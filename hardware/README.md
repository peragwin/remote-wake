# Hardware

## What you need (BOM)
| Qty | Part | Notes |
|-----|------|-------|
| 1 | ESP32-S3 dev board with USB-OTG port exposed | e.g. ESP32-S3-DevKitC-1, Lolin S3 Mini. Two USB ports is convenient (one UART for flashing, one OTG to the PC). |
| 1 | PC817 optocoupler (or any 4-pin photocoupler) | Full galvanic isolation from the motherboard. |
| 1 | 330 Ω resistor | LED-side current limit from 3.3 V GPIO. |
| — | 2.54 mm Dupont wires / headers | To the front-panel `PWR_SW` pins. |

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
