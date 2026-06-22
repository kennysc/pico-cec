#!/usr/bin/env python3
"""
pico-cec-listener.py

Listens on the Pico's serial port for CEC bridge events and reacts:
  - EVT:CEC_TV_STANDBY        -> systemctl suspend (TV turned off via its remote)
  - EVT:READY:PA=<addr>       -> log discovered physical address
  - EVT:ERROR:<msg>           -> log

Also exposes a tiny local control socket (Unix domain socket) so the
systemd-sleep hook and shutdown service can ask this daemon to send
CMD:PWR_ON / CMD:PWR_OFF to the Pico without each of them needing to
open/manage the serial port themselves (avoids port contention).

Protocol (serial, 115200 8N1, newline terminated ASCII):
  PC -> Pico:   CMD:PWR_ON | CMD:PWR_OFF | CMD:PING
  Pico -> PC:   ACK:<cmd> | NACK:<cmd>:<reason> | PONG
                EVT:CEC_TV_STANDBY | EVT:CEC_TV_ON | EVT:CEC_ACTIVE_SOURCE_LOST
                EVT:READY:PA=<a.b.c.d> | EVT:ERROR:<msg>
"""

import json
import logging
import os
import queue
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial  # pyserial

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SERIAL_PORT = os.environ.get("PICO_CEC_PORT", "/dev/pico-cec")  # see udev rule
BAUD_RATE = 115200
RECONNECT_DELAY_S = 3.0
COMMAND_TIMEOUT_S = 5.0  # how long to wait for ACK/NACK after sending a CMD
SERIAL_READ_TIMEOUT_S = 0.1  # keep control commands low-latency for suspend

CONTROL_SOCKET_PATH = "/run/pico-cec/control.sock"

LOG_LEVEL = os.environ.get("PICO_CEC_LOG_LEVEL", "INFO").upper()

logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
)
log = logging.getLogger("pico-cec-listener")


# ---------------------------------------------------------------------------
# systemd actions
# ---------------------------------------------------------------------------

def systemctl_suspend():
    log.info("TV reported standby -> suspending system")
    try:
        subprocess.run(["systemctl", "suspend"], check=True)
    except subprocess.CalledProcessError as e:
        log.error("systemctl suspend failed: %s", e)


# ---------------------------------------------------------------------------
# Serial link to the Pico, run on its own thread.
#
# Design: one dedicated thread owns the serial.Serial object exclusively.
# All writes go through a queue so the control-socket thread never touches
# the serial port directly -- avoids the classic "two threads call
# ser.write() at the same time" bug.
# ---------------------------------------------------------------------------

class PicoLink:
    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self.ser: serial.Serial | None = None
        self._write_q: "queue.Queue[tuple[str, queue.Queue]]" = queue.Queue()
        self._command_lock = threading.Lock()
        self._pending_lock = threading.Lock()
        self._pending: dict[str, queue.Queue] = {}  # cmd -> reply queue
        self._last_physical_address: str | None = None
        self._stop = threading.Event()

    @property
    def last_physical_address(self) -> str | None:
        return self._last_physical_address

    def stop(self):
        self._stop.set()

    def run(self):
        """Main loop: connect, read lines, dispatch. Reconnects on failure."""
        while not self._stop.is_set():
            try:
                self._connect_and_pump()
            except (serial.SerialException, OSError) as e:
                log.warning("serial link lost (%s); reconnecting in %.1fs",
                            e, RECONNECT_DELAY_S)
                self.ser = None
                time.sleep(RECONNECT_DELAY_S)

    def _connect_and_pump(self):
        log.info("opening serial port %s @ %d", self.port, self.baud)
        with serial.Serial(self.port, self.baud, timeout=SERIAL_READ_TIMEOUT_S) as ser:
            self.ser = ser
            # Drain any boot-time noise from the Pico
            ser.reset_input_buffer()
            while not self._stop.is_set():
                self._flush_pending_writes()
                line = ser.readline()
                if not line:
                    continue  # read timeout, loop back to check stop/writes
                try:
                    text = line.decode("ascii", errors="replace").strip()
                except Exception:
                    continue
                if text:
                    self._handle_line(text)

    def _flush_pending_writes(self):
        try:
            while True:
                cmd, reply_q = self._write_q.get_nowait()
                self._send_now(cmd, reply_q)
        except queue.Empty:
            pass

    def _send_now(self, cmd: str, reply_q: queue.Queue):
        if self.ser is None:
            reply_q.put(("NACK", "no serial link"))
            return
        with self._pending_lock:
            self._pending[cmd] = reply_q
        try:
            self.ser.write(f"CMD:{cmd}\n".encode("ascii"))
            self.ser.flush()
            log.debug("-> CMD:%s", cmd)
        except (serial.SerialException, OSError) as e:
            with self._pending_lock:
                self._pending.pop(cmd, None)
            reply_q.put(("NACK", str(e)))

    def send_command(self, cmd: str, timeout: float = COMMAND_TIMEOUT_S):
        """Serialize commands so reply matching stays one-at-a-time."""
        with self._command_lock:
            reply_q: queue.Queue = queue.Queue(maxsize=1)
            self._write_q.put((cmd, reply_q))
            try:
                status, detail = reply_q.get(timeout=timeout)
                return status, detail
            except queue.Empty:
                with self._pending_lock:
                    self._pending.pop(cmd, None)
                return "TIMEOUT", f"no reply to {cmd} within {timeout}s"

    def _handle_line(self, text: str):
        log.debug("<- %s", text)

        if text.startswith("EVT:CEC_TV_STANDBY"):
            log.info("event: TV went to standby")
            systemctl_suspend()

        elif text.startswith("EVT:CEC_TV_ON"):
            log.info("event: TV reports power on")

        elif text.startswith("EVT:CEC_ACTIVE_SOURCE_LOST"):
            log.info("event: another device became active source")

        elif text.startswith("EVT:READY:PA="):
            pa = text.split("PA=", 1)[1]
            self._last_physical_address = pa
            log.info("Pico ready, discovered physical address: %s", pa)

        elif text.startswith("EVT:ERROR:"):
            log.warning("Pico reported error: %s", text[len("EVT:ERROR:"):])

        elif text.startswith("ACK:") or text.startswith("NACK:"):
            self._dispatch_reply(text)

        elif text == "PONG":
            self._dispatch_reply(text, cmd="PING")

        else:
            log.debug("unrecognized line from Pico: %s", text)

    def _dispatch_reply(self, text: str, cmd: str | None = None):
        if cmd is None:
            # text looks like "ACK:PWR_ON" or "NACK:PWR_ON:reason"
            parts = text.split(":")
            status = parts[0]
            cmd = parts[1] if len(parts) > 1 else ""
            detail = parts[2] if len(parts) > 2 else ""
        else:
            status, detail = text, ""

        with self._pending_lock:
            reply_q = self._pending.pop(cmd, None)
        if reply_q is not None:
            reply_q.put((status, detail))
        else:
            log.debug("reply for unknown/expired command: %s", text)


# ---------------------------------------------------------------------------
# Local control socket: lets pico-cec-suspend-hook and pico-cec-shutdown
# request PWR_ON / PWR_OFF without managing the serial port themselves.
#
# Wire format: single line JSON in, single line JSON out.
#   request:  {"cmd": "PWR_ON"}
#   response: {"status": "ACK"|"PONG", "detail": ""}
# ---------------------------------------------------------------------------

def run_control_socket(link: PicoLink):
    sock_path = Path(CONTROL_SOCKET_PATH)
    sock_path.parent.mkdir(parents=True, exist_ok=True)
    if sock_path.exists():
        sock_path.unlink()

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(str(sock_path))
    # Allow local manual control commands without requiring root.
    os.chmod(sock_path, 0o666)
    srv.listen(4)
    log.info("control socket listening at %s", sock_path)

    while True:
        conn, _ = srv.accept()
        threading.Thread(
            target=_handle_control_conn, args=(conn, link), daemon=True
        ).start()


def _handle_control_conn(conn: socket.socket, link: PicoLink):
    try:
        conn.settimeout(COMMAND_TIMEOUT_S + 2)
        data = conn.recv(256)
        if not data:
            return
        req = json.loads(data.decode("utf-8").strip())
        cmd = req.get("cmd", "")
        if cmd not in ("PWR_ON", "PWR_OFF", "PING"):
            resp = {"status": "ERROR", "detail": f"unknown cmd {cmd!r}"}
        else:
            status, detail = link.send_command(cmd)
            resp = {"status": status, "detail": detail}
        conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
    except Exception as e:
        log.warning("control socket error: %s", e)
    finally:
        conn.close()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    link = PicoLink(SERIAL_PORT, BAUD_RATE)

    ctl_thread = threading.Thread(target=run_control_socket, args=(link,), daemon=True)
    ctl_thread.start()

    try:
        link.run()
    except KeyboardInterrupt:
        pass
    finally:
        link.stop()


if __name__ == "__main__":
    sys.exit(main())
