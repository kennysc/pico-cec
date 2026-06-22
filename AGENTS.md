# pico-cec

## Layout

```
firmware/   — Pico firmware (C, Pico SDK)
bin/        — PC-side Python scripts
systemd/    — systemd units, udev rules, polkit rule, sleep hook
docs/       — wiring and setup guide
pico-sdk/   — git submodule (https://github.com/raspberrypi/pico-sdk.git)
install.sh  — run as root on Bazzite; expects bin/ and systemd/ subdirs
```

## Build firmware

```bash
cd firmware
export PICO_SDK_PATH=../pico-sdk
mkdir build && cd build
cmake .. && make -j4
# produces pico-cec-bridge.uf2 — copy to Pico in BOOTSEL mode
```

Board is `waveshare_rp2040_zero`. USB stdio only (no UART) → device appears as `/dev/ttyACMx`.

## PC-side install

```bash
sudo ./install.sh
```

Creates `picocec` user, installs pyserial into `/usr/local/lib/pico-cec-venv`, installs systemd units/udev rules/polkit rule/sleep hook. Designed for Bazzite (rpm-ostree) — avoids package layering.

## Serial protocol (115200 8N1, ASCII, newline-terminated)

PC → Pico: `CMD:PWR_ON` / `CMD:PWR_OFF` / `CMD:PING`
Pico → PC: `ACK:<cmd>` / `NACK:<cmd>:<reason>` / `PONG` / `EVT:CEC_TV_STANDBY` / `EVT:READY:PA=<a.b.c.d>`

## Architecture

- **pico-cec-listener** (daemon): owns serial port to Pico, exposes Unix domain socket at `/run/pico-cec/control.sock`. Reacts to `EVT:CEC_TV_STANDBY` by calling `systemctl suspend`.
- **pico-cec-ctl** (CLI client): connects to the control socket, used by systemd boot/shutdown/sleep hooks.
- No direct serial access from hooks — always through the daemon's socket.

## Key gotchas

- `cec_transceiver.c` is a working reimplementation — do not overwrite it from upstream without reconciling the interface in `cec_transceiver.h`.
- GP16 is hardwired to onboard WS2812 LED — do not repurpose for anything else.
- HPD pin (GP3) needs a 10k/20k resistor divider if wired (5V signal). Default `CEC_HPD_NOT_WIRED` is safe.
- udev rule uses stock Pico SDK VID:PID `2e8a:000a` — verify with `udevadm info -a -n /dev/ttyACM0` before relying on it.
- CEC RX: edge-IRQ driven. CEC TX: alarm-IRQ driven. PIO not implemented (stretch goal).
- On suspend: systemd-sleep hook runs `pico-cec-ctl.py PWR_OFF` (pre) then `PWR_ON` (post).
- On shutdown: `pico-cec-shutdown.service` uses `ExecStop` + `RemainAfterExit=true` to send `PWR_OFF`.
