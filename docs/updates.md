# Firmware updates and the embedding boundary

Status: proposed OTA design, 2026-10-22. The local ROM-bootloader maintenance
API is implemented and host-tested separately; physical integration is pending.
There is no automatic network updater,
release-signing pipeline or update-announcement service yet.

## Ownership

The proposed `remote-wake` feature is a reusable **whole-application updater**. It never tries
to replace its own component inside a linked firmware image. An embedding app
supplies identity, release source, trusted publisher keys, partition layout and
application health checks. Updating the dependency means building and releasing
a new complete application.

| Owner | Responsibility |
| --- | --- |
| remote-wake component | Bounded update state machine, manifest verification, HTTPS streaming, inactive-slot installation, restart coordination, rollback bookkeeping and progress/status API |
| Standalone remote-wake app | Its product/board/profile identity, partitions, public release keys, default update policy and boot health |
| Embedding app such as Voidstats | Its own identity, partitions, release keys/repository, local display/progress UI, restart preparation and health checks |
| Relay | Advertise matching releases on connect and when a channel changes; cache metadata and report progress |
| Publisher CI | Build each complete image, test it, publish GitHub release assets and signed manifests, then promote the channel |

This keeps the public base useful to other projects without baking Voidwell,
screen drivers or one repository into it. A public standalone device must never
install a Voidstats image just because both contain remote-wake or use ESP32-S3.

## Release identity

Match an explicit tuple before downloading or writing anything:

```
product / board / profile / channel / partition_layout
```

Example products:

- `remote-wake / esp32-s3-devkitc / hid-power / stable / rw-4m-ab-v1`
- `remote-wake / esp32-c3 / power-only / stable / rw-4m-ab-v1`
- `voidstats / lilygo-t-display-s3 / st7789-gc9a01-v1 / stable / vs-16m-ab-v1`

The product owner chooses these stable identifiers. Chip ID, minimum flash and
PSRAM capacity, image length and updater API compatibility are additional checks.
Embedded image metadata must match the signed manifest, not just a release asset
filename. Display the human version but compare a monotonically increasing
release counter scoped to the identity tuple; never order version strings.

Firmware advertises the application version separately from the remote-wake
component version. The existing `RW_FW_VERSION` alone is insufficient for an
embedding app's identity.

## Distribution and notification

1. CI builds the application for its named profile and runs its regressions.
2. Publish the app-only `.bin`, a signed manifest and human release notes as
   versioned GitHub Release assets. USB bootstrap packages may additionally
   contain bootloader/partition images, but OTA never installs those files.
3. After assets exist and their hashes are verified, publish a signed channel
   pointer/catalog revision. A failed publication leaves the previous channel
   revision active. Pin versioned URLs; do not use `/latest/download/app.bin`
   as the identity or integrity check.
4. The relay observes/promotes that revision and announces availability to
   connected devices registered for the matching tuple. Reconnecting devices
   receive the current announcement; an infrequent jittered check recovers
   missed announcements. Offline devices do not need a queued update per release.
5. An enrolled device on automatic stable updates verifies the announcement's
   referenced manifest, downloads to the inactive slot, verifies the image,
   waits for active wake/power work to finish, then restarts itself.

The relay carries metadata, not megabytes of firmware. GitHub serves the binary.
Public release assets require no GitHub token on a device. The downloader must
handle bounded HTTPS redirects while continuing TLS verification; GitHub's
release-asset endpoint can return either the content or a redirect.

Current relay code ignores unsolicited device hello metadata other than pending
command responses. Implementation must explicitly add bounded capability/identity
registration and versioned update announcements. Do not reinterpret an unsigned
announcement as one of the existing authenticated keyboard/power commands. Old
devices continue operating and simply do not advertise update support.

Proposed announcement shape (not implemented protocol):

```json
{
  "update_available": {
    "v": 1,
    "product": "voidstats",
    "board": "lilygo-t-display-s3",
    "profile": "st7789-gc9a01-v1",
    "channel": "stable",
    "manifest_url": "https://github.com/OWNER/REPO/releases/download/v0.2.0/voidstats-manifest.json"
  }
}
```

The receiver only follows manifest locations within its configured release
source. Relay messages are hints: a compromised relay must not gain firmware
signing authority. Publisher keys are distinct from phone command keys and
device relay tokens. Forks and embedding apps supply their own publisher keys.

## Signed manifest contract

Use the existing Ed25519 implementation for publisher verification. Define a
new domain-separated signature format and test vectors, independent of phone
command signatures. One practical envelope is a bounded base64url payload plus
key ID and signature: verify the exact decoded UTF-8 payload bytes prefixed by
`remote-wake-firmware-v1\n`, then parse its strict schema. Reject duplicate keys,
unexpected types, oversized strings and excessive nesting. This avoids implicit
JSON reserialization differences between CI and firmware.

The signed payload includes:

- schema version and minimum updater API;
- the exact product/board/profile/channel/layout identity;
- chip, flash/PSRAM requirements, application version and release counter;
- image URL, byte length and SHA-256 digest;
- publication/expiry times, checked after trusted clock synchronization;
- signing-key generation and any explicitly supported compatibility interval.

Verify publisher signature and compatibility before the download. Stream and
hash the received bytes, enforce the declared size and inactive-slot capacity,
validate the ESP image/header and embedded identity, then select the new slot.
Use HTTPS with certificate validation throughout. A digest from an unsigned
relay message alone is not authentication.

Keep signing credentials in the publisher's protected CI environment, outside
the repository and device image. Key rotation ships overlapping trust sets in
an already-trusted release before changing publishers. Never accept replacement
trust roots solely from the relay.

## Installation, rollback and policy

Proposed public API: initialize with application-supplied configuration, offer a
manifest, read a bounded status snapshot, subscribe to progress, prepare for
restart, and report application health after boot. Callbacks must not perform
network work or rendering inside USB/network callbacks. One worker owns the
update; streaming buffers and retries are bounded.

Use ESP-IDF A/B application slots and `otadata`, with bootloader rollback enabled.
Write only the inactive app slot. After the new app boots, confirm it with
`esp_ota_mark_app_valid_cancel_rollback()` only after required local checks pass.
Failure or an unconfirmed reset returns to the prior valid image. The app owns
its checks: Voidstats checks display initialization/transfers as well as shared
NVS/crypto/USB startup. A temporarily absent PC or Internet connection alone
must not invalidate an otherwise healthy firmware.

Persist the confirmed release high-water mark only after successful boot, and
record rejected image hashes so an unchanged channel cannot cause an endless
install/rollback loop. Keep settings migrations readable by the rollback image.
Downloads interrupted by power/network loss leave the running slot intact.
Back off and jitter retries; publish pending/downloading/verifying/restarting/
healthy/rolled-back/failed states without exposing credentials.

Automatic stable-channel installation is the intended Voidwell policy. The
generic library remains disabled until its application supplies identity and
publisher trust. Public products expose automatic, notify-only and pinned
policies; enrolling in auto updates authorizes future compatible releases
without a confirmation prompt for every version. Do not reboot during an active
keyboard action or power pulse. Downloading can continue while the old app runs;
the short restart is deferred until the app is ready. Updating this controller
does not reboot the desktop PC.

## Partition migration

Existing remote-wake and Voidstats builds have one factory app slot, so they
cannot safely acquire this mechanism through an ordinary app-only OTA update.
Bootstrap the A/B layout once over USB, preserving each device's existing NVS
offset and size. Routine OTA must never rewrite the partition table, bootloader
or pairing/replay state.

- Standalone 4 MB: design two approximately 1.9 MB slots and reserve `otadata`;
  check actual firmware size/headroom in CI before approving the layout.
- Voidstats 16 MB: two 6 MB slots fit. Preserve its NVS at `0x9000/0x5000` and
  existing legacy filesystem region while selecting final offsets. A candidate
  is OTA0 `0x10000/0x600000`, otadata `0x800000/0x2000`, OTA1
  `0x810000/0x600000`; inventory flash and validate the bootstrap before adoption.

The current factory partition files remain unchanged by this plan. A/B migration
and bootloader rollback configuration belong to each product's release, not a
hidden side effect of importing the component.

## Implementation milestones

1. Generic local bootloader entry plus the Voidstats host helper; prove a complete
   software-entry → flash → normal-boot cycle on the physical board.
2. Finalize identity/manifest test vectors, product A/B bootstrap layouts and
   local health/rollback tests. Test bad signatures, wrong products/profiles,
   truncated images, out-of-space, stale releases and power loss at each stage.
3. Implement the shared updater and application adapters; exercise a local
   signed release and forced rollback before attaching GitHub/relay automation.
4. Add release CI and cached relay announcements. Publish a canary release,
   verify normal wake behavior and rollback, then promote the stable channel.

## Primary references

- [ESP-IDF OTA and rollback](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/api-reference/system/ota.html)
- [ESP-IDF HTTPS OTA](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/api-reference/system/esp_https_ota.html)
- [GitHub release asset downloads](https://docs.github.com/en/rest/releases/assets)
