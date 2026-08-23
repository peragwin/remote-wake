import { describe, expect, it } from "vitest";

import { constantTimeEqualHex, sha256Hex } from "../src/crypto";
import { bearerToken } from "../src/http";
import { DEFAULT_BUCKET, consume, newBucket } from "../src/ratelimit";
import { parseRoute } from "../src/routes";

describe("parseRoute", () => {
  it("parses the three v1 routes", () => {
    expect(parseRoute("/v1/send/0123456789abcdef")).toEqual({
      kind: "send",
      deviceId: "0123456789abcdef",
    });
    expect(parseRoute("/v1/device/0123456789abcdef")).toEqual({
      kind: "device",
      deviceId: "0123456789abcdef",
    });
    expect(parseRoute("/v1/presence/0123456789abcdef")).toEqual({
      kind: "presence",
      deviceId: "0123456789abcdef",
    });
  });

  it("rejects unknown shapes", () => {
    expect(parseRoute("/")).toBeNull();
    expect(parseRoute("/v1/send")).toBeNull();
    expect(parseRoute("/v2/send/0123456789abcdef")).toBeNull();
    expect(parseRoute("/v1/bogus/0123456789abcdef")).toBeNull();
    expect(parseRoute("/v1/send/0123456789abcdef/extra")).toBeNull();
  });

  it("flags malformed device ids", () => {
    expect(parseRoute("/v1/send/0123456789ABCDEF")).toBe("bad_device_id");
    expect(parseRoute("/v1/send/short")).toBe("bad_device_id");
    expect(parseRoute("/v1/send/0123456789abcdeff")).toBe("bad_device_id");
  });
});

describe("bearerToken", () => {
  const req = (h: Record<string, string> = {}) =>
    new Request("https://relay.test/", { headers: h });

  it("extracts the credential case-insensitively", () => {
    expect(bearerToken(req({ authorization: "Bearer abc" }))).toBe("abc");
    expect(bearerToken(req({ authorization: "bearer abc" }))).toBe("abc");
  });

  it("rejects missing, wrong-scheme and empty credentials", () => {
    expect(bearerToken(req())).toBeNull();
    expect(bearerToken(req({ authorization: "Basic abc" }))).toBeNull();
    expect(bearerToken(req({ authorization: "abc" }))).toBeNull();
    expect(bearerToken(req({ authorization: "Bearer   " }))).toBeNull();
  });
});

describe("crypto helpers", () => {
  it("hashes to known SHA-256 vectors", async () => {
    expect(await sha256Hex("")).toBe(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    );
    expect(await sha256Hex("abc")).toBe(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    );
  });

  it("compares digests without short-circuiting on content", async () => {
    const a = await sha256Hex("token-a");
    const b = await sha256Hex("token-b");
    expect(constantTimeEqualHex(a, a)).toBe(true);
    expect(constantTimeEqualHex(a, b)).toBe(false);
    expect(constantTimeEqualHex(a, a.slice(0, -1))).toBe(false);
  });
});

describe("token bucket", () => {
  it("allows the burst then denies", () => {
    const t0 = 1_000_000;
    let state = newBucket(DEFAULT_BUCKET, t0);
    const results: boolean[] = [];
    for (let i = 0; i < 7; i++) {
      const r = consume(state, DEFAULT_BUCKET, t0);
      state = r.state;
      results.push(r.allowed);
    }
    expect(results).toEqual([true, true, true, true, true, false, false]);
  });

  it("refills at 10 per minute", () => {
    const t0 = 1_000_000;
    let state = newBucket(DEFAULT_BUCKET, t0);
    for (let i = 0; i < 5; i++) state = consume(state, DEFAULT_BUCKET, t0).state;
    expect(consume(state, DEFAULT_BUCKET, t0).allowed).toBe(false);

    // 6 s → exactly one token.
    const r = consume(state, DEFAULT_BUCKET, t0 + 6_000);
    expect(r.allowed).toBe(true);
    expect(consume(r.state, DEFAULT_BUCKET, t0 + 6_000).allowed).toBe(false);
  });

  it("never exceeds capacity", () => {
    const t0 = 1_000_000;
    const state = newBucket(DEFAULT_BUCKET, t0);
    const r = consume(state, DEFAULT_BUCKET, t0 + 3_600_000);
    expect(r.state.tokens).toBe(DEFAULT_BUCKET.capacity - 1);
  });
});
