import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";

const TOKEN = "AAAA_test_device_token_0123456789abcdefg";
const OTHER_TOKEN = "BBBB_wrong_token_0123456789abcdefghijkl";

let counter = 0;
/** A fresh 16-hex device id per test, so each gets its own Durable Object. */
function newDeviceId(): string {
  counter += 1;
  return counter.toString(16).padStart(16, "0");
}

function auth(token = TOKEN): HeadersInit {
  return { authorization: `Bearer ${token}` };
}

function envelope(dev: string, id: string, extra: Record<string, unknown> = {}) {
  return {
    v: 1,
    dev,
    id,
    ts: Math.floor(Date.now() / 1000),
    ctr: 1,
    act: "ping",
    args: {},
    kid: "p1",
    sig: "not-inspected-by-the-relay",
    ...extra,
  };
}

async function send(
  dev: string,
  body: unknown,
  token = TOKEN,
): Promise<Response> {
  return SELF.fetch(`https://relay.test/v1/send/${dev}`, {
    method: "POST",
    headers: { ...auth(token), "content-type": "application/json" },
    body: typeof body === "string" ? body : JSON.stringify(body),
  });
}

/** Open the device websocket and wait until it is actually open. */
async function connectDevice(dev: string, token = TOKEN): Promise<WebSocket> {
  const res = await SELF.fetch(`https://relay.test/v1/device/${dev}`, {
    headers: { ...auth(token), upgrade: "websocket" },
  });
  expect(res.status).toBe(101);
  const ws = res.webSocket!;
  ws.accept();
  return ws;
}

/**
 * Auto-respond to every command frame the relay pushes, echoing the id back.
 * Returns the list of raw frames the fake device saw.
 */
function autoRespond(
  ws: WebSocket,
  make: (cmd: Record<string, unknown>) => unknown = (cmd) => ({
    v: 1,
    id: cmd["id"],
    ok: true,
    res: { pong: true },
  }),
): string[] {
  const seen: string[] = [];
  ws.addEventListener("message", (event) => {
    const raw = String(event.data);
    seen.push(raw);
    const parsed = JSON.parse(raw) as Record<string, unknown>;
    if (parsed["ping"] !== undefined) return; // keepalive
    ws.send(JSON.stringify(make(parsed)));
  });
  return seen;
}

describe("router", () => {
  it("404s unknown paths", async () => {
    const res = await SELF.fetch("https://relay.test/nope", { headers: auth() });
    expect(res.status).toBe(404);
    expect(await res.json()).toEqual({ err: "not_found" });
  });

  it("rejects a malformed deviceId", async () => {
    for (const bad of ["ABCDEF0123456789", "0123", "0123456789abcdefg"]) {
      const res = await SELF.fetch(`https://relay.test/v1/presence/${bad}`, {
        headers: auth(),
      });
      expect(res.status).toBe(404);
      expect(await res.json()).toEqual({ err: "bad_device_id" });
    }
  });

  it("requires a bearer token", async () => {
    const dev = newDeviceId();
    const res = await SELF.fetch(`https://relay.test/v1/presence/${dev}`);
    expect(res.status).toBe(401);
    expect(await res.json()).toEqual({ err: "unauthorized" });

    const bad = await SELF.fetch(`https://relay.test/v1/presence/${dev}`, {
      headers: { authorization: "Basic zzz" },
    });
    expect(bad.status).toBe(401);
  });

  it("rejects the wrong method", async () => {
    const dev = newDeviceId();
    const res = await SELF.fetch(`https://relay.test/v1/send/${dev}`, {
      headers: auth(),
    });
    expect(res.status).toBe(405);
    expect(await res.json()).toEqual({ err: "method_not_allowed" });
  });
});

describe("token claim (trust on first use)", () => {
  it("claims on first request and rejects a different token later", async () => {
    const dev = newDeviceId();

    const first = await SELF.fetch(`https://relay.test/v1/presence/${dev}`, {
      headers: auth(),
    });
    expect(first.status).toBe(200);
    expect(await first.json()).toEqual({ online: false, since: null });

    const same = await SELF.fetch(`https://relay.test/v1/presence/${dev}`, {
      headers: auth(),
    });
    expect(same.status).toBe(200);

    const wrong = await SELF.fetch(`https://relay.test/v1/presence/${dev}`, {
      headers: auth(OTHER_TOKEN),
    });
    expect(wrong.status).toBe(403);
    expect(await wrong.json()).toEqual({ err: "forbidden" });
  });

  it("refuses a websocket upgrade with the wrong token", async () => {
    const dev = newDeviceId();
    await SELF.fetch(`https://relay.test/v1/presence/${dev}`, { headers: auth() });

    const res = await SELF.fetch(`https://relay.test/v1/device/${dev}`, {
      headers: { ...auth(OTHER_TOKEN), upgrade: "websocket" },
    });
    expect(res.status).toBe(403);
    expect(res.webSocket).toBeNull();
  });

  it("refuses a send with the wrong token", async () => {
    const dev = newDeviceId();
    await connectDevice(dev);
    const res = await send(dev, envelope(dev, "id-1"), OTHER_TOKEN);
    expect(res.status).toBe(403);
  });
});

describe("presence", () => {
  it("reports online while the socket is up", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);

    const res = await SELF.fetch(`https://relay.test/v1/presence/${dev}`, {
      headers: auth(),
    });
    expect(res.status).toBe(200);
    const body = (await res.json()) as { online: boolean; since: number };
    expect(body.online).toBe(true);
    expect(typeof body.since).toBe("number");

    ws.close(1000, "bye");
  });
});

describe("send → response round trip", () => {
  it("forwards the command verbatim and returns the device's response", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);
    const seen = autoRespond(ws);

    const cmd = envelope(dev, "round-trip-1", { act: "wake" });
    const res = await send(dev, cmd);

    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({
      v: 1,
      id: "round-trip-1",
      ok: true,
      res: { pong: true },
    });

    // The relay must not reserialize: the device sees exactly what was posted.
    const commands = seen.filter((f) => !f.includes('"ping"'));
    expect(commands).toEqual([JSON.stringify(cmd)]);

    ws.close(1000, "bye");
  });

  it("correlates concurrent commands by id", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);
    autoRespond(ws, (cmd) => ({ v: 1, id: cmd["id"], ok: true, res: { echo: cmd["act"] } }));

    const [a, b] = await Promise.all([
      send(dev, envelope(dev, "aaa", { act: "wake" })),
      send(dev, envelope(dev, "bbb", { act: "status" })),
    ]);

    expect(await a.json()).toMatchObject({ id: "aaa", res: { echo: "wake" } });
    expect(await b.json()).toMatchObject({ id: "bbb", res: { echo: "status" } });

    ws.close(1000, "bye");
  });

  it("ignores unsolicited and malformed device frames", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);

    ws.send('{"hello":{"fw":"1.0.0"}}');
    ws.send("not json at all");
    ws.send(JSON.stringify({ v: 1, id: "never-asked-for", ok: true }));

    // Connection survives; a real command still round trips.
    autoRespond(ws);
    const res = await send(dev, envelope(dev, "after-noise"));
    expect(res.status).toBe(200);
    expect(await res.json()).toMatchObject({ id: "after-noise" });

    ws.close(1000, "bye");
  });
});

describe("offline and timeout", () => {
  it("504 device_offline when no socket is attached", async () => {
    const dev = newDeviceId();
    const res = await send(dev, envelope(dev, "offline-1"));
    expect(res.status).toBe(504);
    expect(await res.json()).toEqual({ err: "device_offline" });
  });

  it("504 device_offline after the socket goes away", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);
    ws.close(1000, "bye");
    // Let the close propagate to the DO.
    await new Promise((r) => setTimeout(r, 50));

    const res = await send(dev, envelope(dev, "offline-2"));
    expect(res.status).toBe(504);
    expect(await res.json()).toEqual({ err: "device_offline" });
  });
});

describe("validation", () => {
  it("rejects a body over 4096 bytes", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);
    autoRespond(ws);

    const big = envelope(dev, "too-big", { args: { text: "x".repeat(4200) } });
    const res = await send(dev, big);
    expect(res.status).toBe(413);
    expect(await res.json()).toEqual({ err: "too_large" });

    ws.close(1000, "bye");
  });

  it("accepts a body just under the cap", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);
    autoRespond(ws);

    const base = JSON.stringify(envelope(dev, "just-fits", { args: { text: "" } }));
    const pad = 4096 - base.length;
    const body = envelope(dev, "just-fits", { args: { text: "x".repeat(pad) } });
    expect(JSON.stringify(body).length).toBeLessThanOrEqual(4096);

    const res = await send(dev, body);
    expect(res.status).toBe(200);

    ws.close(1000, "bye");
  });

  it("rejects malformed JSON", async () => {
    const dev = newDeviceId();
    const res = await send(dev, "{not json");
    expect(res.status).toBe(400);
    expect(await res.json()).toEqual({ err: "bad_json" });
  });

  it("rejects a dev/path mismatch", async () => {
    const dev = newDeviceId();
    const other = newDeviceId();
    const res = await send(dev, envelope(other, "mismatch-1"));
    expect(res.status).toBe(400);
    expect(await res.json()).toEqual({ err: "dev_mismatch" });
  });

  it("rejects an envelope with no usable id", async () => {
    const dev = newDeviceId();
    const res = await send(dev, { v: 1, dev, act: "wake" });
    expect(res.status).toBe(400);
    expect(await res.json()).toEqual({ err: "bad_envelope" });
  });

  it("rejects a non-object envelope", async () => {
    const dev = newDeviceId();
    const res = await send(dev, [1, 2, 3]);
    expect(res.status).toBe(400);
    expect(await res.json()).toEqual({ err: "bad_envelope" });
  });
});

describe("rate limiting", () => {
  it("allows the burst then 429s", async () => {
    const dev = newDeviceId();
    const ws = await connectDevice(dev);
    autoRespond(ws);

    const codes: number[] = [];
    for (let i = 0; i < 8; i++) {
      const res = await send(dev, envelope(dev, `rl-${i}`));
      codes.push(res.status);
      await res.body?.cancel().catch(() => {});
    }

    // Burst is 5; refill is 10/min so nothing meaningful refills in-test.
    expect(codes.slice(0, 5).every((c) => c === 200)).toBe(true);
    expect(codes.slice(5).every((c) => c === 429)).toBe(true);

    const last = await send(dev, envelope(dev, "rl-final"));
    expect(await last.json()).toEqual({ err: "rate_limited" });

    ws.close(1000, "bye");
  });
});

describe("single device socket", () => {
  it("supersedes an older socket with close code 4001", async () => {
    const dev = newDeviceId();
    const first = await connectDevice(dev);

    const closed = new Promise<CloseEvent>((resolve) => {
      first.addEventListener("close", (e) => resolve(e as CloseEvent));
    });

    const second = await connectDevice(dev);
    const event = await closed;
    expect(event.code).toBe(4001);

    // The newest socket is the one that receives commands.
    autoRespond(second);
    const res = await send(dev, envelope(dev, "to-newest"));
    expect(res.status).toBe(200);
    expect(await res.json()).toMatchObject({ id: "to-newest" });

    second.close(1000, "bye");
  });
});
