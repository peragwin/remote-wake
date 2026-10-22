# Signed whole-application releases

These tools build a signed manifest for a complete ESP-IDF app-only image. They
never flash a device, create production keys, rewrite partition tables, or publish
without an explicit `publish.py --publish` invocation. The updater is a component;
remote-wake standalone and embedding products publish separate complete images.

Use Python 3.10+ with `python -m pip install -r tools/ota/requirements.txt`.
Run `PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s tools/ota/tests -v`.
Tests use an explicitly public RFC 8032 test seed in source and a synthetic ESP
image. This seed is unsuitable for releases. No real publisher keys were created.

## Artifact contract

The application must retain `RW_OTA_DEFINE_IMAGE_IDENTITY` from `ota.h` and
supply that record through updater configuration. `manifest.py inspect --image
firmware.bin` checks the ESP app descriptor, chip header, segment bounds/checksum,
appended image hash and exactly one 236-byte identity record inside app segments.
The ESP application version must also equal the embedded version.
Bootloaders, merged flash packages, trailing data and missing/duplicate identities
are rejected. Initial tooling supports ESP32, C2, C3, C6, H2 and S3 app images;
secure-boot signature-block images need a separately reviewed extension.

The embedded record begins with 16 bytes `RWOTA-IDENTITY1!`, followed by
little-endian uint32 format 1, updater API 1 and release counter. Zero-padded
NUL-terminated fields follow: product[32], board[32], profile[48], channel[16],
layout[32], version[32], chip[16]. See `fixtures/manifest-v1.json` for exact bytes,
public key, signed envelope and deterministic cross-language verification vector.

`identity.json` contains exactly product, board, profile, channel, layout and chip.
Examples (each is an independent release identity):

| Product | Board | Profile | Layout | Chip |
| --- | --- | --- | --- | --- |
| remote-wake | esp32-s3-devkitc | hid-power | rw-4m-ab-v1 | esp32s3 |
| remote-wake | esp32-c3 | power-only | rw-4m-ab-v1 | esp32c3 |
| voidstats | lilygo-t-display-s3 | st7789-gc9a01-v1 | vs-16m-ab-v1 | esp32s3 |

Use channel `stable` or a separately enrolled channel. These layout identifiers
must match actual product partitions; naming a layout does not migrate hardware.
The product build selects its chip/profile, defines the full image identity and
runs its regressions before passing app.bin to the signing job. Never pass a
component archive, factory merged image, or another product's binary.

`payload.json` has exactly these 18 fields (example values are illustrative):

```json
{"v":1,"min_updater":1,"product":"remote-wake","board":"esp32-s3-devkitc","profile":"hid-power","channel":"stable","layout":"rw-4m-ab-v1","chip":"esp32s3","min_flash":4194304,"min_psram":0,"version":"1.2.3","release":7,"url":"https://github.com/OWNER/REPO/releases/download/v1.2.3/app.bin","size":1234567,"sha256":"REPLACE_WITH_64_LOWERCASE_HEX","published":1700000000,"expires":1800000000,"key_generation":1}
```

Generate both unsigned input files from the embedded record and exact app.bin:

```sh
python tools/ota/manifest.py prepare --image app.bin \
  --url https://github.com/OWNER/REPO/releases/download/v1.2.3/app.bin \
  --min-flash 4194304 --min-psram 0 --published 1700000000 --expires 1800000000 \
  --key-generation 1 --output payload.json --identity-output identity.json
```

Use actual current publication/expiry times. `prepare` derives identity, version,
release, size and SHA-256 from the image. It never signs or publishes. All numeric fields are integers;
release and key generation are positive uint32 values. The release counter must
increase for its exact product/board/profile/channel/layout tuple; version strings
are human labels. Publication/expiry are uint32 Unix seconds; expiry follows
publication, is later than verification time, and publication permits at most
300 seconds of future clock skew. Firmware requires a trusted clock. Minimum
updater is currently exactly 1. Identifier lengths and URL bounds match `ota.h`:
product/board/layout/version 31, profile 47, channel/chip 15, HTTPS URL 511 bytes.
Unknown fields, duplicate keys, non-finite numbers and incorrect types fail.

The envelope is exactly `{v:1,kid,payload,sig}`. Payload and signature are unpadded,
canonical base64url. Ed25519 signs `remote-wake-firmware-v1\n` followed by the exact
decoded UTF-8 payload bytes. Verification authenticates bytes before parsing;
canonical JSON is a publisher convenience, not a verifier reserialization rule.
Payloads are bounded to 3072 bytes, envelopes to 6144. Publisher keys are distinct
from phone/device command keys. Key ID and generation must match configured trust.

## Offline sign and verify

Keep an unencrypted Ed25519 PKCS8 PEM private key in an owner-readable-only file
outside the checkout, or supply it from a protected publisher environment.
There is deliberately no key-generation command. Provision/rotate keys through
the product's separate trust process; never put key files in Git, assets or logs.
The private-key loader rejects checkout paths and group/world permissions.

```sh
python tools/ota/manifest.py sign --key /secure/publisher.pem --kid publisher1 \
  --key-generation 1 --current-release 6 --identity identity.json \
  --payload payload.json --image app.bin --output manifest.json \
  --source-prefix https://github.com/OWNER/REPO/releases/download/
python tools/ota/manifest.py verify --key /secure/publisher-public.pem --kid publisher1 \
  --key-generation 1 --current-release 6 --identity identity.json \
  --manifest manifest.json --image app.bin \
  --source-prefix https://github.com/OWNER/REPO/releases/download/
```

Use `--now` only for deterministic offline tests; normal verification uses current
UTC time. Signing rejects mismatches between payload, hash/size and embedded image
identity. No values are inferred from an asset filename. Minimum flash/PSRAM are
product-declared requirements and still need matching hardware validation.

## Explicit publication and atomic channel promotion

`publish.py` defaults to a dry run and prints a plan. It requires a verified public
key, exact identity, a pre-existing reviewed source tag, and authenticated GitHub
CLI access. Add `--publish` only for the authorized publication operation:

```sh
python tools/ota/publish.py --public-key /secure/publisher-public.pem --kid publisher1 \
  --key-generation 1 --current-release 6 --identity identity.json \
  --manifest manifest.json --image app.bin --repository OWNER/REPO --tag v1.2.3 \
  --channel-branch ota --source-prefix https://github.com/OWNER/REPO/releases/download/
```

Prepare the `ota` metadata branch separately. Publication creates a new draft
release with app.bin and manifest.json, downloads and verifies both uploaded
assets, then publishes that immutable versioned release. Finally it writes the
same signed envelope at
`channels/<product>/<board>/<profile>/<channel>/<layout>.json` on the metadata
branch using one GitHub Contents API compare-and-swap commit. The signed URL
always addresses the versioned app.bin, never `latest/download`. Existing channel
metadata is signature/identity checked and rejects stale counters. A concurrent
promotion conflict preserves the old channel pointer; upload/download failures
never promote. A versioned release may remain orphaned after failed promotion.
After that signed pointer succeeds, the publisher merges the tuple into
`catalog.json` on the same metadata branch and performs another atomic CAS commit.
This is the relay's unsigned availability catalog:

```json
{"v":1,"releases":[{"product":"remote-wake","board":"esp32-s3-devkitc","profile":"hid-power","channel":"stable","partition_layout":"rw-4m-ab-v1","version":"1.2.3","release_counter":7,"manifest_url":"https://github.com/OWNER/REPO/releases/download/v1.2.3/manifest.json"}]}
```

Catalog promotion preserves other tuples, rejects duplicate/stale tuple releases,
bounds the catalog to 64 releases/64 KiB and retries concurrent CAS conflicts at
most three times by rereading/merging. Failure leaves the old valid catalog; the
signed periodic pointer may already expose the published release. Configure relay
`UPDATE_CATALOG_URL=https://raw.githubusercontent.com/OWNER/REPO/ota/catalog.json`
and `UPDATE_ALLOWED_REPOS=OWNER/REPO`. The catalog is a hint, never signing authority.
No release asset is overwritten and no source tag is created/moved by the tool.
A rotated signing key requires the publication host to retain old verification
trust or explicitly migrate the channel under review; this initial publisher
accepts one configured key at a time.

## Protected reusable workflow

`.github/workflows/firmware-release.yml` has only `workflow_call`, not a push
trigger. A product-owned, explicitly dispatched build workflow first builds/tests
its exact standalone S3 `hid-power`, C3 `power-only`, or embedding profile and
uploads an artifact containing **app.bin, payload.json, identity.json**. Its
publisher job then calls this reusable workflow with artifact_name, release_tag,
key_id, key_generation, current_release, channel_branch and publisher_ref.
Pin both the workflow `uses: ...@<commit>` and publisher_ref to the same reviewed
40-character commit. The reusable job checks out publisher tools at that pin;
it publishes to the caller's repository, so Voidstats keeps its own release assets,
identity and publisher trust. Do not invoke from untrusted pull-request builds.

Configure the caller repository's `firmware-publisher` environment with required
reviewers, permitted release refs, and environment-only `FIRMWARE_SIGNING_KEY_PEM`.
Pass that secret explicitly to the reusable call. The environment approval is the
publication gate; the workflow cannot create its own repository protection rules.
The job grants contents:write only to publication, runs contract tests, signs,
offline verifies, publishes assets, then promotes the channel. A temporary 0600
key file lives under RUNNER_TEMP and is removed in an always-run cleanup step.
No production key or secret was created by adding this workflow.

Primary references: [Ed25519 API](https://cryptography.io/en/latest/hazmat/primitives/asymmetric/ed25519/)
and [GitHub reusable workflows](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows).

### Concrete standalone build and caller

From an ESP-IDF 5.4 environment, build an opt-in S3 A/B application in a fresh
configuration (use `esp32c3` and its defaults for the power-only profile):

```sh
idf.py -C firmware -B build-ota-s3 \
  -D SDKCONFIG=build-ota-s3/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.ota.defaults' \
  -D PROJECT_VER=1.1.0 set-target esp32s3 build
```

Set `CONFIG_APP_OTA_BOARD`, `CONFIG_APP_OTA_PROFILE`, `CONFIG_APP_OTA_CHANNEL` and
`CONFIG_APP_OTA_RELEASE` deliberately before each release. The release number must
increase for the tuple; `PROJECT_VER` must match the embedded/application version.
Run firmware host regressions and target builds before uploading the artifact.
Use `manifest.py prepare` on `build-ota-s3/remote-wake.bin`, rename that application
image to `app.bin`, and upload it with the prepared payload/identity files using
`actions/upload-artifact`. The tool enforces known slot capacities: 0x1e0000 for
rw-4m-ab-v1, 0x600000 for vs-16m-ab-v1. Other layout owners must validate their own
capacity before signing; the device always checks its real inactive partition.

A product workflow with an explicit `workflow_dispatch` trigger and successful
`build` job can call the publisher as follows (replace the pin and release values):

```yaml
publish:
  needs: build
  permissions:
    contents: write
  uses: peragwin/remote-wake/.github/workflows/firmware-release.yml@REVIEWED_40_CHARACTER_COMMIT
  with:
    publisher_ref: REVIEWED_40_CHARACTER_COMMIT
    artifact_name: standalone-s3-release
    release_tag: v1.1.0-s3
    key_id: publisher-1
    key_generation: 1
    current_release: 0
    channel_branch: ota
  secrets:
    FIRMWARE_SIGNING_KEY_PEM: ${{ secrets.FIRMWARE_SIGNING_KEY_PEM }}
```

A C3 build uses its own artifact, identity/profile and versioned tag. Voidstats
uses its own repository build with board lilygo-t-display-s3, profile
st7789-gc9a01-v1, layout vs-16m-ab-v1 and application version 0.2.0 (or its next
reviewed release). Both supply only the publisher public key to firmware; the
private signing secret is available only in the protected publishing job.
The caller must create/review its source tag and metadata branch separately.
No publication, secret provisioning or device update has been performed here.

### Device source URLs

For periodic signed-pointer fallback configure the device with
`manifest_url=https://github.com/OWNER/REPO/raw/refs/heads/ota/channels/PRODUCT/BOARD/PROFILE/CHANNEL/LAYOUT.json`
and `source_prefix=https://github.com/OWNER/REPO/`. The broader repository prefix
allows both that initial manifest path and immutable `/releases/download/...`
URLs. Allow exact redirect hosts `raw.githubusercontent.com`,
`release-assets.githubusercontent.com`, `objects.githubusercontent.com` and
`github.com` as needed. Do not give the device a raw.githubusercontent.com initial
manifest URL while configuring a github.com-only source prefix. Relay catalog
entries point directly to the versioned github.com signed-manifest assets, so
they satisfy the same prefix. Redirects remain bounded and HTTPS/TLS verified.
Signing CLI examples can use the narrower releases/download prefix because they
validate image URLs only, not the periodic pointer URL.
