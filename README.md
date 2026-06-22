# pico-cec-bridge

A Raspberry Pi RP2040 (Waveshare RP2040-Zero) that sits inline with your
HDMI cable and bridges CEC between your TV and a Linux HTPC. Lets the PC
wake the TV, switch inputs, and suspend itself when you turn the TV off
with its remote.

- CEC physical address is discovered **dynamically via EDID** — plug into
  any HDMI port, it just works.
- On `PWR_ON`: wakes TV + switches to this input (sends `<Image View On>`
  then `<Active Source>` with the discovered address).
- On `PWR_OFF`: sends `<Standby>` broadcast.
- Detects TV standby → triggers `systemctl suspend`.

---

## Hardware needed

| Item | Notes |
|------|-------|
| Waveshare RP2040-Zero | Any RP2040 board works with pin changes |
| HDMI breakout board | Male+female passthrough to tap signals |
| Female-to-female jumper wires | 4-5 wires |
| 10kΩ + 20kΩ resistors | Only if wiring HPD (optional) |
| USB-C cable | Data, to connect Pico to PC |

## Wiring

| HDMI pin | Signal | RP2040-Zero GPIO |
|----------|--------|-------------------|
| 13 | CEC | GP2 |
| 15 | DDC SCL | GP4 (I2C0 SCL) |
| 16 | DDC SDA | GP5 (I2C0 SDA) |
| 17 | GND | GND |
| 19 (optional) | HPD | GP3 (via 10k/20k divider) |

```
HDMI breakout               RP2040-Zero
─────────────               ───────────
pin 13 (CEC) ───────────────GP2
pin 15 (DDC SCL) ───────────GP4
pin 16 (DDC SDA) ───────────GP5
pin 17 (GND) ───────────────GND
pin 19 (HPD) ──[10k]────┬──GP3
                        [20k]
                         │
                        GND
```

The Pico sits inline: **PC → breakout → TV**. CEC/DDC/GND are tapped off
the passthrough; video signals pass straight through. The Pico connects to
the PC via USB for serial communication.

HPD is optional — skip those wires and the firmware works fine (it
re-discovers the physical address automatically before every `PWR_ON`).

**No level shifters needed.** CEC and DDC are open-drain; the Pico drives
them by pulling GPIOs low (output) or releasing them (input). 3.3V logic
coexists safely with the 5V bus pull-ups. HPD is the only signal that can
be actively driven to 5V, hence the resistor divider.

> GP16 is the onboard WS2812 LED — do not wire anything to it.

---

## Build firmware

```bash
cd firmware
export PICO_SDK_PATH=../pico-sdk
mkdir build && cd build
cmake .. && make -j4
```

Output: `firmware/build/pico-cec-bridge.uf2`

Board target is `waveshare_rp2040_zero`. USB stdio only (no UART).

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

- Creates an unprivileged `picocec` system user (in the `dialout` group)
- Installs `pyserial` into a dedicated venv at `/usr/local/lib/pico-cec-venv`
- Installs udev rule → stable symlink `/dev/pico-cec`
- Installs polkit rule → lets `picocec` call `systemctl suspend`
- Installs and enables systemd units:
  - `pico-cec-listener.service` — long-running daemon, owns the serial
    port, suspends on TV standby
  - `pico-cec-boot.service` — fires at graphical session start, sends
    `PWR_ON`
  - `pico-cec-shutdown.service` — fires on shutdown/reboot, sends `PWR_OFF`
- Installs `/etc/systemd/system-sleep/50-pico-cec.sh` — sends `PWR_OFF`
  before suspend, `PWR_ON` after resume

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

Check for `EVT:READY:PA=<a.b.c.d>` in the logs — the physical address
should be non-zero and match whatever HDMI port the cable is plugged into.

---

## Project layout

```
firmware/        — Pico firmware (C, Pico SDK)
  main.c         — Application: serial protocol + CEC actions
  cec_transceiver.c/h — CEC bit-banging RX/TX + EDID discovery
  CMakeLists.txt — Build config (waveshare_rp2040_zero, USB stdio)
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
wiring rationale, known gaps, and troubleshooting.
