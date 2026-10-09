#!/bin/sh
#
# install.sh -- put the cart side in place. Idempotent, run as root.
#
# It does NOT enable anything, on purpose. Enabling cartdial on a board that is not actually wired to
# a node means the board runs its payloads and then powers itself off, and only a human with physical
# access can undo that. Pass --enable when the board really is a cart.
#
# Installs:  /usr/local/lib/cart/{cartbeat,cartdial,window.d/*}
#            /etc/systemd/system/{cartbeat,cartdial,cart-halt}.service
#            /etc/cart/cart.conf   (commented defaults, only if absent)

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "install.sh: must be root" >&2; exit 1; }

ENABLE=0
for a in "$@"; do
    case $a in
    --enable) ENABLE=1 ;;
    *) echo "install.sh: unknown option $a" >&2; exit 2 ;;
    esac
done

install -d -m 755 /usr/local/lib/cart /usr/local/lib/cart/window.d /etc/cart
install -m 755 "$HERE/cartbeat" "$HERE/cartdial" /usr/local/lib/cart/
for p in "$HERE"/window.d/*; do
    [ -f "$p" ] || continue
    install -m 755 "$p" /usr/local/lib/cart/window.d/
done
for u in cartbeat.service cartdial.service cart-halt.service; do
    install -m 644 "$HERE/systemd/$u" /etc/systemd/system/
done
if [ ! -f /etc/cart/cart.conf ]; then
    cat > /etc/cart/cart.conf <<'CONF'
# cart settings -- uncomment to override. Read by cartbeat and cartdial through the units.
#
#CART_BEAT_PIN=17          # BCM number. 17 (header pin 11) idles LOW, which matters: an undriven pin
#                          # near the node's input threshold is how you invent edges you did not send
#CART_BEAT_INTERVAL=1      # seconds between edges; the node wants 1-5 and tolerates 15s of silence
#CART_LINGER_MAX=3600      # how long a logged-in session may hold the rail open
#CART_DEADLINE=5400        # the whole window, inside the node's 2h backstop
#CART_DIAL_TRIES=3         # dial attempts, because the first races the modem's registration
#CART_DIAL_GAP=60
CONF
    chmod 644 /etc/cart/cart.conf
    echo "wrote /etc/cart/cart.conf (all commented; the defaults are in the scripts)"
fi
systemctl daemon-reload
echo "installed cartbeat, cartdial, cart-halt and the payloads"

warn() { echo "  WARNING: $*"; }
echo "checks:"
PIN=17
command -v pinctrl >/dev/null 2>&1 || warn "pinctrl is missing -- cartbeat cannot drive the pin"
if command -v pinctrl >/dev/null 2>&1; then
    echo "  GPIO$PIN now: $(pinctrl get $PIN 2>&1)"
    case "$(pinctrl get $PIN 2>&1)" in
    *" a"[0-9]*) warn "GPIO$PIN is in an ALT mode -- something else owns it" ;;
    esac
fi
grep -qE "^dtoverlay=w1-gpio" /boot/firmware/config.txt 2>/dev/null &&
    warn "dtoverlay=w1-gpio is enabled: its default pin is GPIO4 and it CLAIMS the line -- keep it off a cart"
grep -qE "^dtoverlay=gpio-poweroff" /boot/firmware/config.txt 2>/dev/null &&
    warn "dtoverlay=gpio-poweroff is enabled: that driver claims its GPIO, and absence of beat already covers the halt"

CLIENT=/usr/local/lib/dialout/dialout-client
if [ -x "$CLIENT" ]; then
    CFG=/usr/local/lib/dialout/dialout.$(hostname).cfg
    HOLD=$(sed -n 's/^hold-max=\([0-9]*\).*/\1/p' "$CFG" 2>/dev/null | tail -1)
    IDLE=$(sed -n 's/^idle=\([0-9]*\).*/\1/p' "$CFG" 2>/dev/null | tail -1)
    [ -n "${HOLD:-}" ] && [ "$HOLD" -gt 5400 ] &&
        warn "dialout hold-max=$HOLD exceeds the node's 2h backstop -- a held tunnel would be hard-cut; 5400 suits a cart"
    [ -n "${IDLE:-}" ] && [ "$IDLE" -gt 900 ] &&
        warn "dialout idle=$IDLE is expensive on a solar cart -- 600 is a better fit"
    systemctl is-enabled dialout-client.timer >/dev/null 2>&1 &&
        warn "dialout-client.timer is enabled -- a cart's schedule is the node's interval: systemctl disable --now dialout-client.timer"
else
    warn "no dialout client at $CLIENT -- the window would have no payload"
fi

if [ "$ENABLE" = 1 ]; then
    systemctl enable cartbeat.service cartdial.service >/dev/null
    echo "ENABLED. cartbeat starts at next boot and cartdial will halt this board when its window ends."
    echo "To take a board back for bench work: touch /etc/cart/inhibit"
else
    echo
    echo "NOT enabled (deliberately). When the board really is wired to a node:"
    echo "    systemctl enable cartbeat.service cartdial.service"
    echo "Try it first without the halt:"
    echo "    systemctl start cartbeat.service && /usr/local/lib/cart/cartdial --no-halt"
    echo "Bench escape hatch, honoured before any halt:"
    echo "    touch /etc/cart/inhibit        # survives reboots"
    echo "    touch /run/cart-inhibit        # this boot only"
fi
echo "done"
