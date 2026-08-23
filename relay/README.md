# relay

Zero-knowledge rendezvous relay: Cloudflare Worker + one Durable Object per
device. Implements the transport section of `docs/protocol.md` and nothing
more — it cannot forge or replay commands (see `docs/security.md`).

```sh
npm install
npm test          # vitest
npx wrangler deploy
```

Endpoints:
- `GET  /v1/device/:deviceId` — WebSocket upgrade for the device (Bearer deviceToken)
- `POST /v1/send/:deviceId`   — phone submits a signed command, long-polls the response (10 s)
- `GET  /v1/presence/:deviceId` — online state

First connection for an unknown deviceId claims it: the DO stores a SHA-256
of the presented token; later connections must match (constant-time).
