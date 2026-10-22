import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import { RESPONSE_TIMEOUT_MS } from "../src/device";

const TOKEN = "AAAA_timeout_token_0123456789abcdef";
const DEV = "00000000deadbe01";

describe("long-poll timeout", () => {
  it(
    "504 device_timeout when the device never answers",
    async () => {
      const upgrade = await SELF.fetch(`https://relay.test/v1/device/${DEV}`, {
        headers: { authorization: `Bearer ${TOKEN}`, upgrade: "websocket" },
      });
      expect(upgrade.status).toBe(101);
      const ws = upgrade.webSocket!;
      ws.accept(); // Deliberately silent: receives the command, never replies.

      const started = Date.now();
      const res = await SELF.fetch(`https://relay.test/v1/send/${DEV}`, {
        method: "POST",
        headers: {
          authorization: `Bearer ${TOKEN}`,
          "content-type": "application/json",
        },
        body: JSON.stringify({
          v: 1,
          dev: DEV,
          id: "never-answered",
          ts: Math.floor(Date.now() / 1000),
          ctr: 1,
          act: "wake",
          args: {},
          kid: "p1",
          sig: "x",
        }),
      });

      expect(res.status).toBe(504);
      expect(await res.json()).toEqual({ err: "device_timeout" });
      expect(Date.now() - started).toBeGreaterThanOrEqual(RESPONSE_TIMEOUT_MS - 1_000);

      ws.close(1000, "bye");
    },
    RESPONSE_TIMEOUT_MS + 10_000,
  );

  it("waits for a valid 12-second power hold to complete", async () => {
    const dev = "00000000deadbe02";
    const upgrade = await SELF.fetch(`https://relay.test/v1/device/${dev}`, {
      headers: { authorization: `Bearer ${TOKEN}`, upgrade: "websocket" },
    });
    const ws = upgrade.webSocket!;
    ws.accept();
    ws.addEventListener("message", (event) => {
      const command = JSON.parse(event.data as string);
      if (command.id === "long-hold") {
        setTimeout(() => ws.send(JSON.stringify({ v: 1, id: command.id,
          ok: true, res: { ms: 12000 } })), 12_000);
      }
    });
    try {
      const response = await SELF.fetch(`https://relay.test/v1/send/${dev}`, {
        method: "POST",
        headers: { authorization: `Bearer ${TOKEN}`, "content-type": "application/json" },
        body: JSON.stringify({ dev, id: "long-hold", act: "power_hold", args: { ms: 12000 } }),
      });
      expect(response.status).toBe(200);
      expect(await response.json()).toEqual({ v: 1, id: "long-hold", ok: true,
        res: { ms: 12000 } });
    } finally {
      ws.close(1000, "bye");
    }
  }, RESPONSE_TIMEOUT_MS + 10_000);
});
