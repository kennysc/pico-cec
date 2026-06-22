# Debugging Guide

## Quick check — get serial output

The Pico uses USB CDC serial (`/dev/ttyACM*`). You need to be in the
`dialout` group to access it:

```bash
sudo usermod -aG dialout $USER
# Log out and back in, or run: newgrp dialout
```

Connect the Pico via USB, find the device node:

```bash
ls /dev/ttyACM*
# Or watch dmesg while plugging: dmesg -w
```

Open a serial connection with `picocom` (recommended, simpler than screen):

```bash
picocom -b 115200 /dev/ttyACM0
# Exit: Ctrl+A then Ctrl+X
```

Type commands blindly (no echo) — press Enter, then type `CMD:PING`, press
Enter. Expect `PONG` back.

On replug/reset you should see lines like:
```
EVT:READY:PA=1.0.0.0
AUTO_POWER_ON_FAILED
ACK:PING
```

If no output at all, check:

- **Is the device node right?** `ls /dev/ttyACM*` — may be ttyACM1, ttyACM2, etc.
- **Is the Pico enumerating?** `dmesg -w` while plugging — should show USB connect + tty creation.
- **Is another process holding the port?** `sudo lsof /dev/ttyACM0`
- **Is the user in the `dialout` group?** `groups $USER`.

## Common issues

### No serial output at all
- Pico didn't flash correctly — re-flash the `.uf2` (hold BOOTSEL while plugging, copy file)
- Wrong ttyACM device — try each one
- Another process owns the port — `sudo lsof /dev/ttyACM0`

### TV doesn't turn on (AUTO_POWER_ON_FAILED or ERROR)
- **EDID read failed** (`ERROR:edid_read_failed`) — DDC wiring issue or HDMI not connected. Check GP4 (SCL) and GP5 (SDA) connections to HDMI pins 15 and 16. Verify GND is connected (HDMI pin 17). Confirm the HDMI cable is plugged into both the breakout board and the TV.
- **CEC TX failed** (`NACK:PWR_ON:cec_tx_failed`) — CEC bus issue. Check GP2 connection to HDMI pin 13. Does the TV support CEC? Is CEC enabled in the TV settings? Also can be a secondary symptom of EDID failure (no physical address → can't send valid CEC frames).
- **No EVT:READY** — the Pico never resolved a physical address. Same DDC wiring or HDMI connection suspect.

### PC wakes when plugging in
Normal — the GPU detects a display via HPD (HDMI pin 19). Not a bug.

### TV turns off on PC shutdown but doesn't turn on at boot
- The `pico-cec-listener.service` may not be running at boot time yet — check with `systemctl status pico-cec-listener`
- Or the `pico-cec-boot.service` may have run before the Pico was ready — check logs: `journalctl -u pico-cec-boot.service`

## Testing the Pico in isolation

Without the daemon or PC-side install, you can:

1. Connect serial terminal
2. Type `CMD:PING` — expect `PONG`
3. Type `CMD:PWR_ON` — expect `ACK:PWR_ON` and TV should wake
4. Type `CMD:PWR_OFF` — expect `ACK:PWR_OFF`

If `PING` works but `PWR_ON` doesn't, the CEC hardware connection is the issue. If `PING` fails, the firmware is stuck or crashed — check boot messages.

## Wiring verification

| HDMI pin | Signal | Pico GPIO | Check with multimeter |
|----------|--------|-----------|----------------------|
| 13 | CEC | GP2 | ~2.5V idle (pulled up on TV side) |
| 15 | DDC SCL | GP4 | ~3.3V idle (I2C pull-up) |
| 16 | DDC SDA | GP5 | ~3.3V idle (I2C pull-up) |
| 17 | GND | GND | 0V — continuity to Pico GND |
| 19 | HPD (optional) | GP3 (via divider) | Check with TV connected |

No level shifters needed for CEC/DDC (open-drain, Pico pulls low only). HPD needs 10k/20k divider if wired — it can be 5V.

## Hardware checklist

- [ ] Pico gets power (LED on)
- [ ] USB data connected to PC
- [ ] HDMI breakout: PC → breakout → TV
- [ ] CEC (GP2) to HDMI pin 13
- [ ] DDC SCL (GP4) to HDMI pin 15
- [ ] DDC SDA (GP5) to HDMI pin 16
- [ ] GND to HDMI pin 17
- [ ] HPD only if wired: GP3 via 10k/20k divider to HDMI pin 19
- [ ] TV CEC enabled in settings (may be called "HDMI-CEC", "Anynet+", "Bravia Sync", etc.)
