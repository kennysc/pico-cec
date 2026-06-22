#!/bin/bash
# install.sh - installs the PC-side Pico CEC bridge on Bazzite.
#
# Bazzite is image-based (rpm-ostree): /usr is read-only and owned by the
# base image, but /etc and /var (and /usr/local, which is a symlink-ish
# overlay target on most rpm-ostree systems -- verify with `findmnt /usr/local`)
# are writable. This script only ever writes to /etc, /var, and
# /usr/local/bin, and never layers an RPM package unless python3-pyserial
# truly isn't available another way -- see the venv approach below, which
# avoids an rpm-ostree layer + reboot entirely.
#
# Run as root: sudo ./install.sh

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "run as root (sudo ./install.sh)" >&2
    exit 1
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$SRC_DIR/bin"
SYSTEMD_DIR="$SRC_DIR/systemd"

echo "==> creating picocec system user"
if ! id picocec &>/dev/null; then
    useradd --system --no-create-home --shell /usr/sbin/nologin picocec
fi
# The udev rule (99-pico-cec.rules) sets GROUP="picocec" on the serial
# device, so the picocec user gets access through its primary group.

echo "==> installing python deps into a dedicated venv (no rpm-ostree layering needed)"
PICO_CEC_VENV=/usr/local/lib/pico-cec-venv
if [[ ! -d "$PICO_CEC_VENV" ]]; then
    python3 -m venv "$PICO_CEC_VENV"
fi
"$PICO_CEC_VENV/bin/pip" install --upgrade pip pyserial --quiet

echo "==> installing scripts to /usr/local/bin"
install -Dm755 "$BIN_DIR/pico-cec-listener.py" /usr/local/bin/pico-cec-listener.py
install -Dm755 "$BIN_DIR/pico-cec-ctl.py" /usr/local/bin/pico-cec-ctl.py

# Re-point the listener's shebang at the venv interpreter so it can import
# pyserial without relying on a system package.
sed -i "1s|.*|#!$PICO_CEC_VENV/bin/python3|" /usr/local/bin/pico-cec-listener.py

echo "==> installing udev rule"
install -Dm644 "$SYSTEMD_DIR/99-pico-cec.rules" /etc/udev/rules.d/99-pico-cec.rules
udevadm control --reload-rules
udevadm trigger

echo "==> installing polkit rule"
install -Dm644 "$SYSTEMD_DIR/49-pico-cec-suspend.rules" \
    /etc/polkit-1/rules.d/49-pico-cec-suspend.rules

echo "==> installing systemd units"
install -Dm644 "$SYSTEMD_DIR/pico-cec-listener.service" \
    /etc/systemd/system/pico-cec-listener.service
install -Dm644 "$SYSTEMD_DIR/pico-cec-boot.service" \
    /etc/systemd/system/pico-cec-boot.service
install -Dm644 "$SYSTEMD_DIR/pico-cec-shutdown.service" \
    /etc/systemd/system/pico-cec-shutdown.service

echo "==> installing systemd-sleep hook"
install -Dm755 "$SYSTEMD_DIR/50-pico-cec.sh" \
    /etc/systemd/system-sleep/50-pico-cec.sh

echo "==> reloading systemd, enabling units"
systemctl daemon-reload
systemctl enable --now pico-cec-listener.service
systemctl enable pico-cec-boot.service
systemctl enable --now pico-cec-shutdown.service

echo "==> done"
echo
echo "Verify the Pico shows up as /dev/pico-cec:"
echo "    ls -l /dev/pico-cec"
echo
echo "Watch listener logs:"
echo "    journalctl -u pico-cec-listener.service -f"
echo
echo "Manual test commands:"
echo "    pico-cec-ctl.py PING"
echo "    pico-cec-ctl.py PWR_ON"
echo "    pico-cec-ctl.py PWR_OFF"
