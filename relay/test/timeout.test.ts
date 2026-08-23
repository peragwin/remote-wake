import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";

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
      expect(Date.now() - started).toBeGreaterThanOrEqual(9_000);

      ws.close(1000, "bye");
    },
    20_000,
  );
});
