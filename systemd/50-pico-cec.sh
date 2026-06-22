#!/bin/bash
# /etc/systemd/system-sleep/50-pico-cec.sh
#
# systemd-sleep hook. Invoked automatically by systemd-suspend.service /
# systemd-hibernate.service etc with two args: "$1/$2" = pre|post, then
# suspend|hibernate|hybrid-sleep|suspend-then-hibernate.
#
# pre + suspend  -> about to sleep: tell TV to go to standby
# post + suspend -> just woke up: tell TV to power on + switch input
#
# Must be executable (chmod 0755) and owned by root. systemd-sleep ignores
# non-executable files in this directory, so a bad chmod fails silently --
# verify with: systemctl status systemd-suspend.service after a test run,
# or run `systemd-sleep suspend` manually as root to see hook output.

set -euo pipefail

ACTION="$1"
TARGET="$2"

CTL=/usr/local/bin/pico-cec-ctl.py
LOG_TAG="pico-cec-sleep-hook"

log() {
    logger -t "$LOG_TAG" "$*"
}

case "$ACTION/$TARGET" in
    pre/*)
        log "system entering $TARGET, requesting TV standby"
        if ! "$CTL" PWR_OFF; then
            log "WARNING: pico-cec-ctl PWR_OFF did not ACK (continuing sleep anyway)"
        fi
        ;;
    post/*)
        log "system resumed from $TARGET, requesting TV power on"
        # Small delay: give the GPU a moment to re-establish the HDMI link
        # and for the Pico to notice HPD / re-read EDID if it tracks that,
        # before we ask it to drive CEC.
        sleep 1
        if ! "$CTL" PWR_ON; then
            log "WARNING: pico-cec-ctl PWR_ON did not ACK"
        fi
        ;;
    *)
        log "unhandled action/target: $ACTION/$TARGET"
        ;;
esac

exit 0
