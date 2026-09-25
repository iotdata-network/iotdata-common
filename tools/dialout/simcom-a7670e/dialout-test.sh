#!/bin/bash
# dialout-test.sh -- check a SIMCom A76XX-class modem on the Pi's header UART, then prove the data
# path end to end. Run it when you wire a board up, and again whenever the board changes network.
#
#   dialout-test.sh [--dev /dev/serial0] [--baud 115200] [--target www.google.com]
#                   [--apn APN] [--user U] [--password P] [--no-auth]
#                   [--keep 20] [--no-data] [--no-modem-test]
#                   [--operator MCCMNC | --auto-operator] [--lte-only | --auto-rat]
#
# Defaults suit the Lebara UK SIM: APN uk.lebara.mobi, user wap, password wap. The APN is stored in
# the modem (it survives a reset), so setting it once is enough -- but the script re-asserts it.
#
# Phases, because the Pi has ONE UART: while pppd owns it, no AT command can run (this kernel has no
# CMUX/n_gsm). So it surveys the modem, tests reachability through the MODEM's own IP stack, then
# dials PPP and tests from the host, then re-queries the modem -- which also proves a clean hang-up.
#
# The modem-stack test is the one that tells you where a fault lies: if it fails too, the problem is
# the SIM/APN/network, not ppp. It is advisory ("note" in the summary, never a FAIL): the modem's
# embedded stack is not the path dialout uses, and it can sulk while ppp works perfectly. On this
# A7670E firmware +CPING returns nothing at all, so treat +CDNSGIP as the trustworthy one. The give-away for a wrong APN is a context that activates and gets an
# IP but comes back from AT+CGCONTRDP with no gateway and no DNS -- it carries nothing.
#
# It never touches this box's default route: pppd runs with nodefaultroute and every host-side test
# is bound to ppp0, so running this over ssh on eth0 or wifi is safe.
#
# Roaming: the A76XX has no data-roaming switch. Roaming is a registration state (+CEREG stat 5) and
# whether data then works is down to the SIM's plan and the visited network. This reports the state
# and compares the SIM's IMSI prefix with the network it is on. If automatic selection picks a poor
# network abroad, pin one with --operator (Sweden: 24001 Telia, 24002 Tre, 24007 Tele2, 24008
# Telenor) and/or --lte-only. AT+CPOL? lists the operators the SIM itself prefers.
#
set -u

DEV=/dev/serial0 BAUD=115200 TARGET=www.google.com KEEP=20
APN=uk.lebara.mobi USER=wap PASS=wap
DATA=1 MODEMTEST=1 OPER='' RAT=''
while [ $# -gt 0 ]; do
    case $1 in
    --dev) DEV=$2; shift ;;
    --baud) BAUD=$2; shift ;;
    --target) TARGET=$2; shift ;;
    --apn) APN=$2; shift ;;
    --user) USER=$2; shift ;;
    --password) PASS=$2; shift ;;
    --no-auth) USER='' PASS='' ;;
    --keep) KEEP=$2; shift ;;
    --no-data) DATA=0 ;;
    --no-modem-test) MODEMTEST=0 ;;
    --operator) OPER=$2; shift ;;
    --auto-operator) OPER=auto ;;
    --lte-only) RAT=38 ;;
    --auto-rat) RAT=2 ;;
    -h | --help) awk 'NR>1 && /^#/ { sub(/^# ?/,""); print; next } NR>1 { exit }' "$0"; exit 0 ;;
    *) echo "dialout-test: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ "$(id -u)" = 0 ] || { echo "dialout-test: must be root" >&2; exit 1; }

TMP=$(mktemp -d /run/dialout-test.XXXXXX)
PPPID='' ROUTES='' RESULTS=''
DEFROUTE_BEFORE=$(ip -4 route show default)

cleanup() {
    [ -n "$PPPID" ] && kill "$PPPID" 2>/dev/null
    for r in $ROUTES; do ip route del "$r" 2>/dev/null; done
    rm -rf "$TMP"
    return 0
}
trap cleanup EXIT INT TERM

f() { printf '  %-20s %s\n' "$1" "$2"; }
pass() { RESULTS="$RESULTS|PASS $1"; }
fail() { RESULTS="$RESULTS|FAIL $1"; }
note() { RESULTS="$RESULTS|note $1"; }
summary() {
    echo "== summary"
    printf '%s\n' "$RESULTS" | tr '|' '\n' | sed '/^$/d;s/^/  /'
    printf '%s\n' "$RESULTS" | tr '|' '\n' | grep -q '^FAIL' && exit 1
    exit 0
}

# ---- AT layer. Reads to a deadline, so slow answers (+CPING, +CDNSGIP, COPS) are not missed. -----
at_open() {
    stty -F "$DEV" "$BAUD" cs8 -cstopb -parenb raw -echo -crtscts min 0 time 10 2>/dev/null ||
        { echo "dialout-test: cannot configure $DEV" >&2; exit 1; }
    exec 3<>"$DEV" || { echo "dialout-test: cannot open $DEV" >&2; exit 1; }
    while IFS= read -r -t 1 _junk <&3; do :; done
    return 0
}
at_close() { exec 3>&- 2>/dev/null; return 0; }
at() {                                  # at <cmd> [deadline-seconds] [glob terminator]
    local cmd=$1 secs=${2:-5} term=${3:-} end line out=''
    printf '%s\r' "$cmd" >&3
    end=$(( $(date +%s) + secs ))
    while [ "$(date +%s)" -lt "$end" ]; do
        IFS= read -r -t 2 line <&3 || continue
        line=${line%$'\r'}
        [ -z "$line" ] && continue
        out="$out$line
"
        [ -n "$term" ] && case $line in $term) break ;; esac
        case $line in
        ERROR | "+CME ERROR"* | "+CMS ERROR"*) break ;;
        OK) [ -n "$term" ] || break ;;
        esac
    done
    printf '%s' "$out"
}
v() { printf '%s\n' "$1" | sed -n "s/^$2: //p" | head -1; }
one() { printf '%s' "$1" | tr '\n' ' ' | sed 's/  */ /g;s/ $//'; }

reg_text() {
    case $1 in
    0) echo "not registered, not searching" ;;  1) echo "registered, HOME network" ;;
    2) echo "not registered, searching" ;;      3) echo "registration DENIED" ;;
    5) echo "registered, ROAMING" ;;            *) echo "unknown (stat $1)" ;;
    esac
}
act_text() {
    case $1 in
    0) echo GSM ;; 2) echo UTRAN ;; 3) echo "GSM/EGPRS" ;; 4) echo "UTRAN/HSDPA" ;;
    5) echo "UTRAN/HSUPA" ;; 6) echo "UTRAN/HSPA" ;; 7) echo LTE ;; 9) echo "LTE Cat-M" ;;
    10) echo "LTE NB-IoT" ;; *) echo "act $1" ;;
    esac
}
op_name() {
    case $1 in
    23415) echo "(Vodafone UK)" ;; 23410) echo "(O2 UK)" ;; 23420) echo "(Three UK)" ;;
    23430 | 23433) echo "(EE UK)" ;; 24001) echo "(Telia SE)" ;; 24002) echo "(Tre SE)" ;;
    24007) echo "(Tele2 SE)" ;; 24008) echo "(Telenor SE)" ;; *) echo "" ;;
    esac
}

# =================================================================================================
echo "== dialout-test $(date -u '+%Y-%m-%dT%H:%M:%SZ') on $(hostname)"
if ip link show ppp0 >/dev/null 2>&1; then
    echo "dialout-test: ppp0 already exists -- stop the dialout client / pppd first" >&2; exit 1
fi

echo "== 1. modem"
at_open
case "$(at AT 5)" in
*OK*) f "port" "$DEV @ $BAUD, responding"; pass "AT" ;;
*) f "port" "$DEV @ $BAUD -- NO RESPONSE"; fail "AT"
   echo "     check: TX/RX crossed (pin 8 -> module RXD, pin 10 <- module TXD), common ground,"
   echo "     module powered and its power-key state, 3.3V levels, baud (try --baud 9600)"
   at_close; summary ;;
esac
at ATE0 >/dev/null; at 'AT+CMEE=2' >/dev/null

[ -n "$OPER" ] && { [ "$OPER" = auto ] &&
    f "operator select" "automatic: $(one "$(at 'AT+COPS=0' 120)")" ||
    f "operator select" "$OPER: $(one "$(at "AT+COPS=1,2,\"$OPER\"" 120)")"; }
[ -n "$RAT" ] && f "radio pref" "CNMP=$RAT: $(one "$(at "AT+CNMP=$RAT" 30)")"

ident=$(at ATI 5)
f "manufacturer" "$(v "$ident" Manufacturer)"
f "model" "$(v "$ident" Model)"
f "revision" "$(v "$ident" Revision)"
f "IMEI" "$(v "$ident" IMEI)"
f "firmware" "$(v "$(at 'AT+CGMR' 5)" '+CGMR')"
f "UART baud" "$(v "$(at 'AT+IPR?' 5)" '+IPR')"
f "functionality" "$(v "$(at 'AT+CFUN?' 5)" '+CFUN')"
f "SIM" "$(v "$(at 'AT+CPIN?' 5)" '+CPIN')"
f "ICCID" "$(v "$(at 'AT+CICCID' 5)" '+ICCID')"
imsi=$(at 'AT+CIMI' 5 | grep -E '^[0-9]{6,}$' | head -1)
f "IMSI" "${imsi:-(none)}"

echo "== 2. network"
cops=$(v "$(at 'AT+COPS?' 10)" '+COPS')
opnum=$(printf '%s' "$cops" | sed -n 's/.*"\([0-9]*\)".*/\1/p')
opact=$(printf '%s' "$cops" | awk -F, '{print $4}')
f "operator" "$opnum $(op_name "$opnum") $( [ -n "$opact" ] && act_text "$opact" )"
cereg=$(v "$(at 'AT+CEREG?' 8)" '+CEREG')
stat=$(printf '%s' "$cereg" | awk -F, '{gsub(/ /,"",$2); print $2}')
f "registration" "$(reg_text "${stat:-4}")  (+CEREG: $cereg)"
case "${stat:-0}" in 1 | 5) pass "registration" ;; *) fail "registration" ;; esac
if [ -n "$imsi" ] && [ -n "$opnum" ]; then
    home=$(printf '%s' "$imsi" | cut -c1-${#opnum})
    [ "$home" = "$opnum" ] && f "home/visited" "on its HOME network $opnum $(op_name "$opnum")" ||
        f "home/visited" "ROAMING: SIM home $home $(op_name "$home") -> on $opnum $(op_name "$opnum")"
fi
csq=$(v "$(at 'AT+CSQ' 5)" '+CSQ'); rssi=${csq%%,*}
[ -n "$rssi" ] && [ "$rssi" != 99 ] && f "signal" "$(( rssi * 2 - 113 )) dBm (+CSQ: $csq)" ||
    f "signal" "unknown (+CSQ: $csq)"
f "attached" "$(v "$(at 'AT+CGATT?' 8)" '+CGATT')"
cpsi=$(v "$(at 'AT+CPSI?' 8)" '+CPSI')
f "system info" "$cpsi"
f "  band" "$(printf '%s' "$cpsi" | awk -F, '{print $7}')"
f "  rsrq/rsrp/rssi/snr" "$(printf '%s' "$cpsi" | awk -F, '{print $11", "$12", "$13", "$14" (raw)"}')"

echo "== 3. data context"
cur=$(at 'AT+CGDCONT?' 8 | grep '^+CGDCONT: 1,' | head -1)
f "context 1 now" "${cur:-(undefined)}"
if [ -n "$APN" ] && ! printf '%s' "$cur" | grep -q "\"$APN\""; then
    f "setting APN" "$APN"
    at 'AT+CGACT=0,1' 20 >/dev/null
    f "  define" "$(one "$(at "AT+CGDCONT=1,\"IP\",\"$APN\"" 10)")"
    f "  activate" "$(one "$(at 'AT+CGACT=1,1' 45)")"
fi
[ -n "$USER" ] && f "context auth" "$USER: $(one "$(at "AT+CGAUTH=1,1,\"$USER\",\"$PASS\"" 10)")"
rdp=$(v "$(at 'AT+CGCONTRDP=1' 10)" '+CGCONTRDP')
f "context detail" "$rdp"
apn_dns=$(printf '%s' "$rdp" | awk -F, '{gsub(/"/,"",$6); print $6}')
if [ -n "$apn_dns" ]; then f "network gave DNS" "$apn_dns"; pass "context carries DNS"
else f "network gave DNS" "NOTHING -- this context carries no DNS/gateway, the APN is likely wrong"
     fail "context carries DNS"; fi
f "address" "$(one "$(at 'AT+CGPADDR=1' 8 | grep '^+CGPADDR')")"

if [ "$MODEMTEST" = 1 ]; then
    echo "== 4. reachability through the modem's own IP stack (independent of ppp)"
    at 'AT+NETCLOSE' 25 >/dev/null          # the socket PDP profile can only be set while closed
    f "socket PDP" "$(one "$(at 'AT+CSOCKSETPN=1' 10)")"
    f "netopen" "$(one "$(at 'AT+NETOPEN' 25)")"
    png=$(at 'AT+CPING="8.8.8.8",1,3,64,1000,8000,255' 45 '+CPING: 3*' | grep '+CPING: 3' | head -1)
    f "modem ping 8.8.8.8" "${png:-no result}"
    recv=$(printf '%s' "$png" | awk -F, '{gsub(/ /,"",$3); print $3}')
    [ -n "$recv" ] && [ "$recv" != 0 ] && pass "modem ping" || note "modem ping -- advisory only"
    gip=$(at "AT+CDNSGIP=\"$TARGET\"" 35 '+CDNSGIP:*' | grep '+CDNSGIP' | head -1)
    f "modem DNS $TARGET" "$gip"
    case "$gip" in "+CDNSGIP: 1,"*) pass "modem DNS" ;; *) note "modem DNS -- advisory only" ;; esac
    at 'AT+NETCLOSE' 25 >/dev/null
fi
at_close
[ "$DATA" = 0 ] && summary

# =================================================================================================
echo "== 5. data path over ppp0 (target $TARGET)"
{
    echo "ABORT 'BUSY'"; echo "ABORT 'NO CARRIER'"; echo "ABORT 'ERROR'"; echo "TIMEOUT 20"
    echo "'' AT"; echo "OK ATE0"
    [ -n "$APN" ] && echo "OK 'AT+CGDCONT=1,\"IP\",\"$APN\"'"
    echo "OK 'ATD*99#'"; echo "CONNECT ''"
} >"$TMP/chat"
set -- "$DEV" "$BAUD" connect "/usr/sbin/chat -v -f $TMP/chat" \
    nodetach noauth nodefaultroute usepeerdns local nocrtscts noipdefault \
    lcp-echo-interval 10 lcp-echo-failure 3 maxfail 1
[ -n "$USER" ] && set -- "$@" user "$USER" password "$PASS"
f "pppd auth" "${USER:-none}"
pppd "$@" >"$TMP/ppp.log" 2>&1 &
PPPID=$!
printf '  dialling'
ip4=''
for i in $(seq 1 60); do
    ip4=$(ip -4 -br addr show ppp0 2>/dev/null | awk '{print $3}')
    [ -n "$ip4" ] && break
    kill -0 "$PPPID" 2>/dev/null || break
    printf '.'; sleep 1
done
echo
if [ -z "$ip4" ]; then
    fail "ppp0 up"; echo "  ppp0 did not come up. pppd log:"; tail -20 "$TMP/ppp.log" | sed 's/^/    /'
    summary
fi
pass "ppp0 up"
f "ppp0 address" "$ip4"
f "peer" "$(ip -4 addr show ppp0 | sed -n 's/.*peer \([0-9.]*\).*/\1/p')"
f "mtu" "$(cat /sys/class/net/ppp0/mtu)"
dns=$(sed -n 's/^nameserver //p' /etc/ppp/resolv.conf 2>/dev/null | tr '\n' ' ')
f "carrier DNS" "${dns:-(none offered)}"
rx0=$(cat /sys/class/net/ppp0/statistics/rx_bytes) tx0=$(cat /sys/class/net/ppp0/statistics/tx_bytes)

# dig cannot bind to a device, so route just that resolver via ppp0 for the query.
ip1=$(printf '%s' "$dns" | awk '{print $1}')
tgt_ip=''
if [ -n "$ip1" ]; then
    ip route add "$ip1/32" dev ppp0 2>/dev/null && ROUTES="$ROUTES $ip1/32"
    tgt_ip=$(dig +time=3 +tries=1 +short "@$ip1" "$TARGET" A 2>/dev/null | grep -E '^[0-9.]+$' | head -1)
fi
if [ -n "$tgt_ip" ]; then f "DNS (carrier)" "$TARGET -> $tgt_ip"; pass "DNS over ppp0"
else f "DNS (carrier)" "no answer via ${ip1:-(no resolver)}"; fail "DNS over ppp0"
     tgt_ip=$(getent ahosts "$TARGET" | awk '$2=="STREAM"{print $1; exit}'); fi

if [ -n "$tgt_ip" ]; then
    png=$(ping -I ppp0 -c 4 -W 3 -q "$tgt_ip" 2>&1)
    loss=$(printf '%s' "$png" | grep -oE '[0-9]+% packet loss')
    rtt=$(printf '%s' "$png" | sed -n 's|.*= \([0-9./]*\) ms|\1|p')
    f "ping $tgt_ip" "${loss:-no result}  rtt min/avg/max/mdev = ${rtt:-n/a} ms"
    case "$loss" in 100%* | "") fail "ping over ppp0" ;; *) pass "ping over ppp0" ;; esac
fi

https=$(curl --interface "if!ppp0" -sS -m 30 -o /dev/null \
    -w 'http %{http_code}  dns %{time_namelookup}s  connect %{time_connect}s  tls %{time_appconnect}s  total %{time_total}s  %{size_download}B at %{speed_download}B/s' \
    "https://$TARGET/" 2>&1)
f "https $TARGET" "$https"
case "$https" in http\ 200* | http\ 3*) pass "https over ppp0" ;; *) fail "https over ppp0" ;; esac

echo "  holding the link for ${KEEP}s:"
end=$(( $(date +%s) + KEEP ))
while [ "$(date +%s)" -lt "$end" ]; do
    sleep 5
    rx=$(cat /sys/class/net/ppp0/statistics/rx_bytes) tx=$(cat /sys/class/net/ppp0/statistics/tx_bytes)
    r=$(ping -I ppp0 -c 1 -W 3 -q "${tgt_ip:-8.8.8.8}" 2>/dev/null | sed -n 's|.*= [0-9.]*/\([0-9.]*\)/.*|\1|p')
    printf '    rx %-9s tx %-9s rtt %s ms\n' "$(( rx - rx0 ))" "$(( tx - tx0 ))" "${r:-timeout}"
done

echo "== 6. hang up, then re-query the modem"
kill "$PPPID" 2>/dev/null; PPPID=''
for i in $(seq 1 20); do ip link show ppp0 >/dev/null 2>&1 || break; sleep 1; done
if ip link show ppp0 >/dev/null 2>&1; then f "ppp0" "still present after 20s"; fail "hangup"
else f "ppp0" "gone"; pass "hangup"; fi
f "ppp session" "$(one "$(grep -E 'Connect time|Sent [0-9]+ bytes' "$TMP/ppp.log")")"
for r in $ROUTES; do ip route del "$r" 2>/dev/null; done; ROUTES=''
sleep 2
at_open
case "$(at AT 8)" in *OK*) f "modem after PPP" "responding"; pass "modem after PPP" ;;
*) f "modem after PPP" "NO RESPONSE"; fail "modem after PPP" ;; esac
f "signal now" "$(v "$(at 'AT+CSQ' 5)" '+CSQ')"
f "registration now" "$(v "$(at 'AT+CEREG?' 8)" '+CEREG')"
f "address now" "$(one "$(at 'AT+CGPADDR=1' 8 | grep '^+CGPADDR')")"
at_close

[ "$(ip -4 route show default)" = "$DEFROUTE_BEFORE" ] &&
    { f "default route" "unchanged"; pass "default route"; } ||
    { f "default route" "CHANGED: was [$DEFROUTE_BEFORE] now [$(ip -4 route show default)]"
      fail "default route"; }
summary
