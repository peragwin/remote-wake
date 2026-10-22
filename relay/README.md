# relay

Zero-knowledge rendezvous relay: Cloudflare Worker + one Durable Object per
device. Implements the Transport section of `docs/protocol.md` and nothing
more — it cannot forge or replay commands (see `docs/security.md`).

```sh
npm install
npm run typecheck   # tsc --noEmit
npm test            # vitest, runs inside workerd via @cloudflare/vitest-pool-workers
npx wrangler deploy
```

## Endpoints

All three require `Authorization: Bearer <deviceToken>`. `:deviceId` must match
`^[0-9a-f]{16}$`. Every error is `{"err": "<code>"}` with a JSON content type.

| Route | Method | Behaviour |
|---|---|---|
| `/v1/device/:deviceId` | GET (Upgrade: websocket) | Device socket. `101` on success. |
| `/v1/send/:deviceId` | POST | Forwards the envelope, long-polls up to 20 s for the device's response, returns it verbatim with `200`. |
| `/v1/presence/:deviceId` | GET | `{"online": bool, "since": <unix seconds>|null}` |

### Error codes

| Status | `err` | When |
|---|---|---|
| 400 | `bad_json` | Body is not parseable JSON. |
| 400 | `bad_envelope` | Body is not a JSON object, or `id` is missing/not a string/over 128 chars. |
| 400 | `dev_mismatch` | Envelope `dev` ≠ path `:deviceId`. |
| 401 | `unauthorized` | Missing or malformed `Authorization` header. |
| 403 | `forbidden` | Token does not match the one that claimed this deviceId. |
| 404 | `not_found` / `bad_device_id` | Unknown path / deviceId is not 16 lowercase hex. |
| 405 | `method_not_allowed` | Wrong verb for the route. |
| 409 | `duplicate_id` | A command with that `id` is already in flight. |
| 413 | `too_large` | Body over 4096 bytes. |
| 426 | `upgrade_required` | `/v1/device` without `Upgrade: websocket`. |
| 429 | `rate_limited` | Token bucket exhausted (burst 5, refill 10/min). |
| 429 | `too_many_pending` | More than 8 commands in flight for this device. |
| 504 | `device_offline` | No device socket attached. |
| 504 | `device_timeout` | Device did not answer within 20 s; the action may already have run. |

## What the relay knows

Per device it stores exactly four values: `SHA-256(deviceToken)` (hex), the unix
ms of the last presence transition, the rate-limit bucket, and a socket
generation counter. **No command or response payload is ever persisted or
logged.** The Worker looks at only two envelope fields — `dev` (must equal the
path) and `id` (used to correlate the response). `v`, `ts`, `ctr`, `act`,
`args`, `kid` and `sig` are never parsed; the command is pushed to the device as
the exact bytes that were POSTed, because the Ed25519 signature covers that
serialization.

## Trust on first use

The first request of any kind for an unknown deviceId claims it: the DO writes
`SHA-256(<presented token>)` to storage. Every later request must present a
token with the same digest, compared with `crypto.subtle.timingSafeEqual` (with
a non-short-circuiting fallback). There is no unclaim — a device that rotates
its token needs a new deviceId, which is what a firmware factory reset produces.

## Keepalive

The device socket is accepted with the **WebSocket Hibernation API**
(`state.acceptWebSocket`), so an idle device costs no Durable Object duration.

- Device → relay: a text frame with the exact payload `ping` is answered `pong`
  by `setWebSocketAutoResponse`, without waking the object at all.
- Relay → device: a DO alarm fires every 25 s and sends the text frame
  `{"v":1,"ping":<unix seconds>}`. **Firmware must ignore any frame that is not
  a command envelope** (this one has no `id`/`act`), and may treat it as
  liveness evidence. Workers cannot emit RFC 6455 control ping frames, so the
  25 s keepalive of `docs/protocol.md` is carried at the application layer.

Only one device socket is attached at a time; a new upgrade closes the previous
socket with code **4001** (`superseded`).

## Limits

- 4096-byte body cap on `/v1/send`, enforced with a bounded read (the overflow
  is drained, never buffered).
- Device → relay frames over 16 KiB are dropped.
- Token bucket per device: burst 5, refill 10/min.
- 8 concurrent in-flight commands per device. In practice the rate limiter binds
  first — the cap is defence in depth against a device that stops answering.

## Deploy

```sh
npm install
npx wrangler login                 # once, per machine
npx wrangler deploy
```

The first deploy applies migration `v1`, which creates the SQLite-backed
`DeviceObject` namespace. Deploys after that are plain code pushes — do not edit
or remove the `[[migrations]]` block.

`wrangler deploy` prints the `*.workers.dev` URL; that is a usable relay URL for
`relayUrl` during pairing. For a stable hostname, add the domain to your
Cloudflare account and uncomment the route block in `wrangler.toml`:

```toml
[[routes]]
pattern = "relay.example.com"
custom_domain = true
```

then redeploy. Cloudflare provisions the certificate; the device connects to
`wss://relay.example.com/v1/device/<deviceId>` and the PWA posts to
`https://relay.example.com/v1/send/<deviceId>`.

There are no secrets to configure — the relay holds no keys, and device tokens
arrive from the devices themselves. `wrangler tail` streams request metadata
only; bodies are never logged.

Local development: `npx wrangler dev` serves the same routes on
`http://127.0.0.1:8787` with a local Durable Object.

## Layout

```
src/index.ts     Worker router (path shape, method, bearer presence) → DO
src/device.ts    DeviceObject: auth, websocket, long-poll, presence, alarm
src/routes.ts    /v1/... path parsing (shared)
src/crypto.ts    SHA-256 hex + constant-time compare
src/ratelimit.ts token bucket
src/http.ts      JSON responses, bearer extraction
test/            vitest: unit tests + integration tests against real workerd
```

## Optional OTA release hints

Release discovery is disabled by default. Operators may set two non-secret
Worker variables in their deployment configuration:

```toml
[vars]
UPDATE_CATALOG_URL = "https://raw.githubusercontent.com/OWNER/REPO/ota-channels/catalog.json"
UPDATE_ALLOWED_REPOS = "OWNER/REPO"
```

Do not put GitHub tokens in these values. The source is public metadata; neither
relay nor device needs a GitHub credential. The allowlist accepts at most 32
comma-separated `OWNER/REPO` entries. The catalog URL must be HTTPS on
`raw.githubusercontent.com` within an allowed repository, or a versioned GitHub
release asset. Redirects are rejected, so raw GitHub is the practical catalog
endpoint. No device-supplied URL is fetched.

The bounded catalog format is:

```json
{"v":1,"releases":[{"product":"remote-wake","board":"esp32-s3-devkitc","profile":"hid-power","channel":"stable","partition_layout":"rw-4m-ab-v1","version":"1.1.0","release_counter":2,"manifest_url":"https://github.com/OWNER/REPO/releases/download/v1.1.0/manifest.json"}]}
```

It permits at most 64 releases and 64 KiB. Identity strings contain ASCII
letters, digits, `.`, `_` or `-`, beginning with a letter or digit. Maximum
lengths are product 31, board 31, profile 47, channel 15, partition layout 31,
and version 31 characters. Counters are unsigned 32-bit integers; versions are
human labels and are never sorted. Manifest URLs are at most 511 bytes and must
refer to versioned `https://github.com/OWNER/REPO/releases/download/TAG/ASSET`
locations within the allowlist, with no credentials, query, fragment or custom
port. Signed manifest fields `layout` and `release` map to catalog
`partition_layout` and `release_counter` respectively.

Configured devices add `hello.update` with `v:1`, `updater_api:1`, the complete
identity tuple, application version and release counter. Only a strictly newer
matching tuple produces `{ "update_available": { "v":1, ...release } }`. Old
clients remain unchanged. WebSocket attachments retain registration and check
state through hibernation. A source check is limited to once per five minutes
per connected device, with a 30-second floor for repeated registration frames.
Public validated metadata is shared through the Cache API for 120 seconds.
Fetches time out after five seconds and cancel oversized streams immediately.
Catalog failures leave the keyboard command channel operating normally.

Announcements are unsigned advisory metadata, separate from authenticated
keyboard/power commands. The device's application-supplied source policy and
publisher signature verification remain authoritative. The relay never serves
firmware binaries or supplies signing keys. Matching newer releases are announced again on each five-minute refresh, so
a hint dropped by a busy updater can recover; reconnecting devices also receive
the current matching release. No deployment or channel promotion happens merely by
building this code.
