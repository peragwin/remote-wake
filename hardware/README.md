# Hardware

## What you need (BOM)
| Qty | Part | Notes |
|-----|------|-------|
| 1 | ESP32-S3 dev board with USB-OTG port exposed | e.g. ESP32-S3-DevKitC-1, Lolin S3 Mini. Two USB ports is convenient (one UART for flashing, one OTG to the PC). |
| 1 | PC817 optocoupler (or any 4-pin photocoupler) | Full galvanic isolation from the motherboard. |
| 1 | 330 Ω resistor | LED-side current limit from 3.3 V GPIO. |
| — | 2.54 mm Dupont wires / headers | To the front-panel `PWR_SW` pins. |
| 1 | Second PC817 + 1 µF capacitor (optional) | Power-LED sense; see below. |

## Wiring

```
 ESP32-S3                       PC817                    Motherboard
 GPIO5 ──[330Ω]── 1 (anode)     4 (collector) ── PWR_SW+ (PANEL header)
 GND ──────────── 2 (cathode)   3 (emitter)  ── PWR_SW− / GND
```

- `PWR_SW+` vs `PWR_SW−`: the collector must go to the *positive* side of the
  header (measure: the pin that sits at ~3.3 V when idle). Swapping them just
  means the "press" does nothing — harmless, flip the two wires.
- The existing case power button stays connected in parallel; both work.
- USB: the S3's **OTG/USB port** (GPIO19/20 native pins on devkits it's the
  connector labeled `USB`, not `UART`) goes to any USB port on the PC. This
  also powers the ESP32 whenever the PSU provides standby power (default for
  soldered USB headers and most rear ports).

## Optional: power-LED sense (`pc_power`)

A second PC817 reads the motherboard's power LED so `status` can report
`pc_power` as `on`, `off` or `sleep` (blinking LED). Enable it with
`CONFIG_RW_PC_SENSE_ENABLED=y` and set `CONFIG_RW_PC_SENSE_GPIO` (default 6).

```
 Motherboard side (LED loop, in series)     ESP32 side
 PWR_LED+ ──► PC817 #2 pin 1 (anode)        pin 4 (collector) ──► GPIO6 (internal pull-up)
              PC817 #2 pin 2 (cathode)                             └─ 1 µF ── GND (optional)
                 └──► case LED + ... case LED − ──► PWR_LED−
                                            pin 3 (emitter) ──► GND
```

- The opto LED goes **in series** with the case's power LED: header `+` →
  opto pin 1, opto pin 2 → case LED `+`, case LED `−` → header `−`. Both carry
  the same current, so the opto sees exactly what the case LED shows. The
  PC817 adds about 1.2 V. On a 5 V header the case LED stays lit, a little
  dimmer. On a 3.3 V header a blue or white case LED (about 3 V) has no
  headroom left: wire the opto alone on the header instead.
- Reversed polarity just reads `off` (and the case LED goes dark). Swap the
  two header wires.
- The transistor pulls the input low while the LED is lit, so the default
  active-low setting fits. The capacitor smooths boards that PWM-dim the LED.
  Without it, a dimmed steady LED can read as `sleep`.
- It's sampled every 10 ms. `on`/`off` need 5 s of steady LED, and `sleep`
  needs four changes in a row, each less than 5 s apart. See
  `docs/protocol.md`.

## Host setup for wake-on-USB
- **BIOS/UEFI**: enable USB wake / ErP off so USB standby power stays on in S3
  sleep (usually default).
- **Windows**: Device Manager → the "remote-wake" HID keyboard → Power
  Management → *Allow this device to wake the computer*.
- **Linux**: most distros enable wakeup for HID keyboards; verify with
  `cat /sys/bus/usb/devices/*/power/wakeup` and set `enabled` via udev if not.
- **Wake from S5 (full power-off)** is what the `power_tap` GPIO path is for —
  USB wake generally only works from sleep (S3/S4 with fast startup).

## Status LED
On-board RGB LED (WS2812 on GPIO48 on DevKitC): breathing = setup mode,
solid dim = connected to relay, blink = executing command, red = auth failure
(rate-limit lockout window).
