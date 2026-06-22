# pico-cec-bridge

A Raspberry Pi RP2040 (Waveshare RP2040-Zero) that sits inline with your
HDMI cable and bridges CEC between your TV and a Linux HTPC. Lets the PC
wake the TV, switch inputs, and suspend itself when you turn the TV off
with its remote.

- Current recommended setup uses a **static CEC physical address** of
  `1.0.0.0` (TV HDMI 1).
- On `PWR_ON`: wakes TV + switches to this input (sends `<Image View On>`
  then `<Active Source>` with the configured address).
- On `PWR_OFF`: sends `<Standby>` broadcast.
- Detects TV standby → triggers `systemctl suspend`.

---

## Hardware needed

| Item | Notes |
|------|-------|
| Waveshare RP2040-Zero | Any RP2040 board works with pin changes |
| HDMI breakout board | Male+female passthrough to tap signals |
| Female-to-female jumper wires | 2-5 wires depending on options |
| 10kΩ + 20kΩ resistors | Only if wiring HPD (optional) |
| USB-C cable | Data, to connect Pico to PC |

## Wiring

| HDMI pin | Signal | RP2040-Zero GPIO |
|----------|--------|-------------------|
| 13 | CEC | GP2 |
| 17 | GND | GND |
| 19 (optional) | HPD | GP3 (via 10k/20k divider) |

```
HDMI breakout               RP2040-Zero
─────────────               ───────────
pin 13 (CEC) ───────────────GP2
pin 17 (GND) ───────────────GND
pin 19 (HPD) ──[10k]────┬──GP3
                        [20k]
                         │
                        GND
```

The Pico sits inline: **PC → breakout → TV**. CEC/GND are tapped off
the passthrough; video signals pass straight through. The Pico connects to
the PC via USB for serial communication.

HPD is optional — skip those wires and the firmware still works fine.

Do not connect HDMI DDC pins 15/16 directly to `GP4`/`GP5` on the current
hardware. On the tested setup the DDC bus idled at about 5.1V; Linux could
read EDID, but direct RP2040 access failed. If you want to retry dynamic EDID
later, add a bidirectional I2C level shifter first.

> GP16 is the onboard WS2812 LED — do not wire anything to it.

---

## Build firmware

```bash
cd firmware
mkdir build && cd build
cmake .. && make -j4
```

Output: `firmware/build/pico-cec-bridge.uf2`

If the `.uf2` side artifact is not emitted automatically in your local build
tree, generate it manually with:

```bash
firmware/build/_deps/picotool/picotool uf2 convert --quiet firmware/build/pico-cec-bridge.elf firmware/build/pico-cec-bridge.uf2 --family rp2040
```

Board target is `waveshare_rp2040_zero`. USB stdio only (no UART). Requires the ARM GCC toolchain (`gcc-arm-none-eabi`).

## Flash

1. Hold the BOOTSEL button on the Pico while plugging it into your PC
2. Copy the `.uf2` file to the mounted RPI-RP2 drive:
   ```bash
   cp firmware/build/pico-cec-bridge.uf2 /media/$USER/RPI-RP2/
   ```
3. The Pico reboots automatically and appears as `/dev/ttyACM0` (or similar)

---

## Install PC-side

Run as root on the Linux PC (designed for Bazzite/rpm-ostree, works on any
distro with systemd):

```bash
sudo ./install.sh
```

This does the following:

- Creates a `picocec` system user
- Installs `pyserial` into a dedicated venv at `/usr/local/lib/pico-cec-venv`
- Installs udev rule → stable symlink `/dev/pico-cec`
- Installs polkit rule (currently stale unless the listener is moved back to `User=picocec`)
- Installs and enables systemd units:
  - `pico-cec-listener.service` — long-running daemon, owns the serial
    port, suspends on TV standby
  - `pico-cec-boot.service` — fires at graphical session start, sends
    `PWR_ON`
  - `pico-cec-shutdown.service` — fires on shutdown/reboot, sends `PWR_OFF`
  - `pico-cec-suspend.service` — fires on suspend/resume, sends `PWR_OFF`
    on suspend entry and `PWR_ON` after resume

> **Verify the udev VID:PID** before relying on the symlink:
> ```bash
> udevadm info -a -n /dev/ttyACM0 | grep -E 'idVendor|idProduct'
> ```
> The default in `systemd/99-pico-cec.rules` is `2e8a:000a` (Pico SDK
> default). Update if your firmware uses a custom VID:PID.

## Verify

```bash
# Confirm the device node exists
ls -l /dev/pico-cec

# Watch listener logs
journalctl -u pico-cec-listener.service -f

# Manual round-trip tests
pico-cec-ctl.py PING       # expect: PONG
pico-cec-ctl.py PWR_ON     # TV should wake + switch input
pico-cec-ctl.py PWR_OFF    # TV should go to standby
```

Check for `EVT:READY:PA=1.0.0.0` in the logs unless you changed the stored PA.

---

## Project layout

```
firmware/        — Pico firmware (C, Pico SDK)
  main.c         — Application: serial protocol + CEC actions
  cec_transceiver.c/h — CEC bit-banging RX/TX + EDID discovery
  CMakeLists.txt — Build config (waveshare_rp2040_zero, USB stdio)
  pico_sdk_import.cmake — Copied from SDK; locates pico-sdk submodule
bin/             — PC-side Python scripts
  pico-cec-listener.py — Daemon: owns serial port, exposes control socket
  pico-cec-ctl.py      — CLI client for the control socket
systemd/         — System configuration
  99-pico-cec.rules         — udev rule for /dev/pico-cec
  49-pico-cec-suspend.rules — polkit rule for suspend
  pico-cec-listener.service
  pico-cec-boot.service
  pico-cec-shutdown.service
  50-pico-cec.sh            — systemd-sleep hook
docs/            — Wiring and setup reference
install.sh       — PC-side install script (run as root)
pico-sdk/        — Git submodule (github.com/raspberrypi/pico-sdk)
```

## More info

See [docs/WIRING_AND_SETUP.md](docs/WIRING_AND_SETUP.md) for detailed
wiring rationale, future EDID retry notes, and troubleshooting.
