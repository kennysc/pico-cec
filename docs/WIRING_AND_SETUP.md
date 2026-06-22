# Pico CEC Bridge — Wiring & Setup

## 1. What this is

A Raspberry Pi RP2040 board — specifically a **Waveshare RP2040-Zero** —
sits inline with the HDMI cable between your Bazzite HTPC and the TV,
tapping CEC + DDC + GND. It:

- Discovers its CEC **physical address dynamically via DDC/EDID** every
  time it needs it — never hardcoded to a port number, so moving the cable
  to a different TV input just works.
- On `PWR_ON`: sends `<Image View On>` then `<Active Source>` (with the
  freshly-discovered physical address) so the TV wakes AND switches to the
  correct input, regardless of which physical HDMI port it's in.
- On `PWR_OFF`: sends `<Standby>` broadcast.
- Listens for `<Standby>` from the TV (logical address 0) and reports it
  to the PC over serial, so the PC can suspend itself when you turn the TV
  off with its own remote.

## 2. Wiring

Board: **Waveshare RP2040-Zero**. Pin numbers below are raw GPIO numbers
(GPx), which on this board are exposed directly on the 23-pin edge header
— confirmed against Waveshare's pinout reference, no conflicts with USB,
onboard flash, or the crystal.

| HDMI pin | Signal | RP2040-Zero GPIO | Notes |
|---|---|---|---|---|
| 13 | CEC | GP2 | Open-drain; **requires 10kΩ pull-up to 3.3V** (TV-side pull-up may not pass through breakout board) |
| 15 | DDC clock (SCL) | GP4 (through bidirectional I2C level shifter) | HDMI DDC idles at 5V. Do not wire directly to RP2040 GPIO if you want to retry EDID discovery. |
| 16 | DDC data (SDA) | GP5 (through bidirectional I2C level shifter) | Same as SCL — use the second channel of the same level shifter. |
| 17 | GND | GND | **Must** be tied — common reference for everything else |
| 19 (optional) | HPD | GP3 | **Needs a resistor divider** (e.g. 10k/20k) — HPD can be driven to 5V on some sources unlike CEC/DDC |
| — | Status LED (optional) | GP16 | **Reserved on this board** — hardwired on-board to the onboard WS2812 (NeoPixel) RGB LED's DIN. Don't wire anything external here. |

```
HDMI breakout                    RP2040-Zero
─────────────                    ───────────

              +3.3V
               │
              ┌┴┐ 10kΩ
              └┬┘
               │
pin 13 (CEC) ──┴──────────────────GP2

pin 15 (DDC SCL) ───────────────────HV1  level shifter  LV1────────────────GP4
pin 16 (DDC SDA) ───────────────────HV2                 LV2────────────────GP5
pin 18 (+5V)    ───────────────────HV
Pico 3V3        ───────────────────────────────────────LV

pin 17 (GND) ──────────────────────GND

pin 19 (HPD, optional) ──[10k]──┬──GP3
                                 [20k]
                                  │
                                 GND
```

If you use a common BSS138-style bidirectional I2C level shifter, one module handles both DDC lines: one channel for SCL and one for SDA. Wire HDMI pin 18 to the shifter HV pin, Pico 3V3 to LV, and share ground with HDMI pin 17.

The Pico sits **inline**: PC HDMI out → breakout board (male/female
passthrough) → TV. CEC/DDC/GND are tapped off the passthrough; video
signal lines pass straight through untouched. The Pico connects to the PC
separately via USB for the serial control link.

If you don't want HPD wiring, skip it — firmware works without it, just
re-discovers physical address less proactively (still safe: it
re-discovers automatically before any `PWR_ON` if its cached address is
unknown/stale).

## 3. DDC voltage levels

CEC is open-drain and can still be wired directly to the Pico as described in
§2. DDC is different: on the tested setup, SCL/SDA idle at about 5.1V and the
TV's EDID is readable by Linux while the Pico's direct GP4/GP5 connection
fails. Treat HDMI DDC as a 5V bus and insert a bidirectional I2C level shifter
before trying EDID discovery again.

The RP2040 drives these pins by switching GPIO direction (input =
released/high, output-low = driven low) rather than actively driving a
high level, but RP2040 GPIO are not formally 5V-tolerant. Do not rely on
direct 5V DDC pull-ups into GP4/GP5.

HPD can be actively driven to 5V by some sources, hence the optional
resistor divider (10k/20k) if you wire that pin.

## 4. Firmware overview

- `main.c` — application layer: serial protocol parsing, CEC action
  dispatch (`PWR_ON`/`PWR_OFF`/`PING`), incoming frame handling.
- `cec_transceiver.c` — CEC bit-banging RX (edge-IRQ driven) and TX
  (alarm-IRQ driven), EDID/DDC physical address discovery over I2C.
- `cec_transceiver.h` — interface for the transceiver layer.
- `CMakeLists.txt` — Pico SDK project, board `waveshare_rp2040_zero`,
  USB stdio only.

CEC RX is edge-interrupt driven; CEC TX uses the hardware timer alarm.
PIO is not used (stretch goal).

## 5. PC-side install

Run from the repo root:

```bash
sudo ./install.sh
```

This:
- Creates an unprivileged `picocec` system user (in the `dialout` group
  for serial access)
- Installs pyserial into a dedicated venv at `/usr/local/lib/pico-cec-venv`
  (avoids `rpm-ostree install` + reboot entirely — venvs live on the
  writable `/usr/local` overlay)
- Installs the udev rule (`/dev/pico-cec` stable symlink)
- Installs the polkit rule (lets `picocec` call `systemctl suspend`)
- Installs and enables:
  - `pico-cec-listener.service` — long-running daemon, owns the serial
    port, suspends the system on `EVT:CEC_TV_STANDBY`
  - `pico-cec-boot.service` — fires once at graphical session start,
    sends `PWR_ON`
  - `pico-cec-shutdown.service` — fires on system shutdown/reboot (via
    `ExecStop`), sends `PWR_OFF`
  - `/etc/systemd/system-sleep/50-pico-cec.sh` — fires on suspend/resume,
    sends `PWR_OFF` before sleep and `PWR_ON` after resume

> **Verify the udev VID:PID** before relying on the symlink:
> ```bash
> udevadm info -a -n /dev/ttyACM0 | grep -E 'idVendor|idProduct'
> ```
> The default in `systemd/99-pico-cec.rules` is `2e8a:000a` (Pico SDK
> default). Update if your firmware uses a custom VID:PID.

## 6. Verifying it works

```bash
# Confirm the device node exists
ls -l /dev/pico-cec

# Watch the listener's logs live
journalctl -u pico-cec-listener.service -f

# Manual round-trip test
pico-cec-ctl.py PING      # expect: PONG
pico-cec-ctl.py PWR_ON    # TV should wake + switch input
pico-cec-ctl.py PWR_OFF   # TV should go to standby
```

Check that `EVT:READY:PA=...` in the logs shows a *sensible, non-zero*
physical address matching wherever you've actually got the cable plugged
in — that's your confirmation dynamic discovery is working.

## 7. Known gaps

1. **PIO-based TX/RX** — stretch goal. The current interrupt-driven
   approach (edge-IRQ RX, alarm-IRQ TX) is field-validated against
   `cec-compliance`. PIO would free up CPU and reduce jitter.
2. ~~**WS2812 status LED** — GP16 is reserved for the onboard LED but no
   driver is implemented yet. Would be useful for visual status (idle
   blue, active green, error red).~~ **Done.** PIO-based driver implemented.
   Colour conventions: blue (booting), green (ready), red (error),
   yellow (TV standby detected).
3. **HPD wiring** — documented but optional. Only needed if you want
   immediate physical address re-discovery on cable move while the PC is
   running (see §2 for the divider circuit).
