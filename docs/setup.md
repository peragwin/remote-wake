# End-to-end setup

## 0. Build the hardware
Follow [`hardware/README.md`](../hardware/README.md): optocoupler between
GPIO5 and the motherboard `PWR_SW` header, S3's OTG USB port into the PC.

## 1. Deploy the relay (once, ~5 min)
```sh
cd relay && npm install && npx wrangler deploy
```
Note the workers.dev URL (or attach your own domain). The relay is
multi-tenant by deviceId — one deployment serves all your devices.

## 2. Flash the firmware
```sh
cd firmware
idf.py set-target esp32s3
idf.py -p <UART port> flash monitor
```
On first boot the console prints the device id and the setup-AP credentials.

## 3. Pair from your phone
1. Host the `app/` directory anywhere static + HTTPS (Cloudflare Pages:
   `npx wrangler pages deploy app`), open it on your phone, **Add to Home
   Screen**, and copy your phone's public key from the pairing screen.
2. Join the `remote-wake-xxxx` Wi-Fi the device broadcasts; open
   `http://192.168.4.1`.
3. Enter your home Wi-Fi, the relay URL, and paste the phone public key.
4. Copy the pairing blob it returns back into the PWA. Device reboots and
   connects; the PWA's presence dot goes green.

## 4. Prepare the PC
Enable *Allow this device to wake the computer* for the new HID keyboard
(see the host section of `hardware/README.md`).

## 5. Use it
- **WAKE** — nudges the host out of sleep via USB.
- **Unlock** — types your stored sequence (read the warning in
  [`security.md`](security.md#properties--mitigations) about the relay seeing
  `type` payloads in v1).
- **Power · tap** — like pressing the button: powers on from full shutdown.
- **Power · hold** — hold the on-screen ring, then confirm: hard power-off
  for a wedged machine.

## Troubleshooting
| Symptom | Check |
|---|---|
| Presence dot grey | Device serial log; Wi-Fi creds; relay URL reachable; re-enter setup (hold BOOT 5 s). |
| `stale` errors | Device couldn't reach SNTP; check network's NTP egress. |
| Wake does nothing | OS wake permission (step 4); BIOS ErP/USB standby power; try wake from sleep vs. full S5 (use power tap for S5). |
| Power tap does nothing | Swap the two optocoupler output wires (polarity). |
| `busy` | A power pulse is already in progress; wait for it to finish. |
