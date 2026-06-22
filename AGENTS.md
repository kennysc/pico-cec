# pico-cec

## Scope

- Ignore `firmware/build/` and `pico-sdk/` when searching for repo logic; `firmware/build/` is generated output and `pico-sdk/` is a submodule.
- The only project-owned code paths are `firmware/`, `bin/`, `systemd/`, `docs/`, and `install.sh`.

## Build And Verify

- Firmware build: `cmake -S firmware -B firmware/build && cmake --build firmware/build -j4`
- Firmware artifact: `firmware/build/pico-cec-bridge.uf2`; if the local build tree does not emit it automatically, generate it with `firmware/build/_deps/picotool/picotool uf2 convert --quiet firmware/build/pico-cec-bridge.elf firmware/build/pico-cec-bridge.uf2 --family rp2040`.
- Board/toolchain assumptions come from `firmware/CMakeLists.txt`: `waveshare_rp2040_zero`, USB stdio enabled, UART stdio disabled, ARM GCC required.
- There is no repo-local automated test/lint setup. Real verification is firmware build plus hardware checks.
- Installed-system smoke tests: `pico-cec-ctl.py PING`, `pico-cec-ctl.py PWR_ON`, `pico-cec-ctl.py PWR_OFF`, and `journalctl -u pico-cec-listener.service -f`.
- Firmware-only debugging bypasses the daemon: connect to `/dev/ttyACM*` at 115200 and send raw `CMD:*` lines.

## Architecture

- `bin/pico-cec-listener.py` is the only process that should own the serial port; everything else talks to `/run/pico-cec/control.sock`.
- `bin/pico-cec-ctl.py` is just a Unix-socket client for the listener. Systemd boot/shutdown/sleep hooks use it specifically to avoid serial-port contention.
- `systemd/pico-cec-shutdown.service` relies on `RemainAfterExit=true` plus `ExecStop=` to send `PWR_OFF` during shutdown.
- `systemd/pico-cec-suspend.service` handles suspend/resume via `suspend.target`: `ExecStart=` sends `PWR_OFF` on suspend entry and `ExecStop=` sends `PWR_ON` after resume.

## Current Protocol Reality

- Firmware supports `CMD:PWR_ON`, `CMD:PWR_OFF`, `CMD:PING`, `CMD:PA`, `CMD:SET_PA:X.X.X.X`, and direct-serial-only `CMD:DISCOVER_PA` in `firmware/main.c`.
- The listener control socket only forwards `PWR_ON`, `PWR_OFF`, and `PING`. `PA` and `SET_PA` work only over direct serial unless you extend both Python scripts.
- Trust `firmware/main.c` over the prose docs for physical-address behavior: current firmware loads PA from flash, defaults to `1.0.0.0` if unset, and can persist a new PA via `CMD:SET_PA` or `bin/make-pa-uf2.py`.
- Current recommended hardware path is static PA only: wire CEC/GND/(optional HPD), leave DDC disconnected unless you add a bidirectional I2C level shifter and are explicitly retrying EDID via direct serial.
- The code still contains EDID-discovery helpers for future debugging, but the current boot path does not call them before advertising readiness or auto-powering on.

## Install Gotchas

- `sudo ./install.sh` is the only install flow. It installs `pyserial` into `/usr/local/lib/pico-cec-venv` and then rewrites the installed `pico-cec-listener.py` shebang to that venv's Python.
- If you add Python dependencies to the listener, update `install.sh`; the service does not rely on system packages.
- `install.sh` creates a `picocec` system user, but `systemd/pico-cec-listener.service` currently has no `User=` line, so the listener runs as root today. The polkit rule and some docs still describe an unprivileged listener; treat those as stale unless you also change the unit.
- The udev rule assumes Pico SDK VID:PID `2e8a:000a`; verify actual hardware with `udevadm info -a -n /dev/ttyACM0` before trusting `/dev/pico-cec`.

## Hardware Constraints

- Do not repurpose GP16; on this board it is hardwired to the onboard WS2812 and used by `ws2812.c`.
- HDMI DDC on pins 15/16 should be treated as a 5V bus. On the tested setup Linux could read EDID while direct RP2040 GP4/GP5 access failed; use a bidirectional I2C level shifter before retrying EDID discovery.
- HPD is optional, but if you wire GP3 it must go through a 10k/20k divider because HDMI HPD can be 5V.
- `cec_transceiver.c` is a repo-specific implementation matched to `cec_transceiver.h`; do not replace it wholesale from upstream without reconciling this interface.
- CEC RX is edge-IRQ driven and CEC TX is alarm-IRQ driven; there is no PIO-based CEC path in this repo.
