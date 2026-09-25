#!/bin/sh
#
# install.sh -- put this modem's image modifications in place. Idempotent, run as root.
#
# Installs:  /etc/ppp/peers/a7670e-lebara   (mode 600, holds the APN password)
#            /etc/ppp/chat/a7670e-lebara
#            /usr/local/sbin/dialout-link   (local path on purpose: the dialout timer runs at boot,
#                                            when /opt may not be mounted)
# Then it checks the things the UART needs, and only warns -- it does not touch boot config.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "install.sh: must be root" >&2; exit 1; }

install -d -m 755 /etc/ppp/peers /etc/ppp/chat /usr/local/sbin
install -m 600 "$HERE/ppp/peers/a7670e-lebara" /etc/ppp/peers/a7670e-lebara
install -m 644 "$HERE/ppp/chat/a7670e-lebara" /etc/ppp/chat/a7670e-lebara
install -m 755 "$HERE/dialout-link" /usr/local/sbin/dialout-link
echo "installed peers, chat and /usr/local/sbin/dialout-link"

warn() { echo "  WARNING: $*"; }
echo "checks:"
command -v pppd >/dev/null || warn "pppd is missing -- apt install ppp"
[ -e /dev/serial0 ] || warn "/dev/serial0 is missing"
CFG=/boot/firmware/config.txt
[ -f "$CFG" ] || CFG=/boot/config.txt
grep -q '^enable_uart=1' "$CFG" 2>/dev/null || warn "enable_uart=1 not in $CFG"
grep -q '^dtoverlay=disable-bt' "$CFG" 2>/dev/null ||
    warn "dtoverlay=disable-bt not in $CFG -- bluetooth may hold the good UART"
if grep -q 'console=serial0\|console=ttyAMA0' /boot/firmware/cmdline.txt /boot/cmdline.txt 2>/dev/null; then
    warn "a serial console is on the modem's UART -- remove console=serial0 from cmdline.txt"
fi
systemctl is-enabled serial-getty@ttyAMA0.service 2>/dev/null | grep -q '^enabled' &&
    warn "serial-getty@ttyAMA0 is enabled -- mask it, or it will fight pppd"
echo "done"
