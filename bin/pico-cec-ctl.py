#!/usr/bin/env python3
"""
pico-cec-ctl.py CMD

Tiny client for the pico-cec-listener control socket. Used by systemd
sleep/shutdown hooks so they don't need to touch the serial port directly
(only the listener daemon owns /dev/pico-cec).

Usage:
    pico-cec-ctl.py PWR_ON
    pico-cec-ctl.py PWR_OFF
    pico-cec-ctl.py PING

Exit codes:
    0  ACK received
    1  NACK / TIMEOUT / ERROR received
    2  could not reach the control socket at all (listener not running)
"""

import json
import socket
import sys

CONTROL_SOCKET_PATH = "/run/pico-cec/control.sock"
TIMEOUT_S = 8.0


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("PWR_ON", "PWR_OFF", "PING"):
        print(f"usage: {sys.argv[0]} PWR_ON|PWR_OFF|PING", file=sys.stderr)
        return 2

    cmd = sys.argv[1]

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(TIMEOUT_S)
            s.connect(CONTROL_SOCKET_PATH)
            s.sendall((json.dumps({"cmd": cmd}) + "\n").encode("utf-8"))
            data = s.recv(256)
    except (OSError, socket.timeout) as e:
        print(f"pico-cec-ctl: cannot reach listener: {e}", file=sys.stderr)
        return 2

    try:
        resp = json.loads(data.decode("utf-8").strip())
    except Exception:
        print(f"pico-cec-ctl: malformed response: {data!r}", file=sys.stderr)
        return 1

    status = resp.get("status", "")
    detail = resp.get("detail", "")
    print(f"{status}{(': ' + detail) if detail else ''}")
    return 0 if status == "ACK" or status == "PONG" else 1


if __name__ == "__main__":
    sys.exit(main())
