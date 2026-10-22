import { SELF, runInDurableObject, evictDurableObject } from "cloudflare:test";
import { env } from "cloudflare:workers";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { Env, Env as RelayEnv } from "../src/types";
import type { DeviceObject } from "../src/device";
import { fetchCatalog, matchingRelease, MAX_CATALOG_BYTES, parseCatalog, registration, updateSource,
  type UpdateAttachment } from "../src/updates";

declare global { namespace Cloudflare { interface Env extends RelayEnv {} } }
const catalog = "https://raw.githubusercontent.com/example/firmware/main/channel.json";
const source = updateSource({ UPDATE_CATALOG_URL: catalog, UPDATE_ALLOWED_REPOS: "example/firmware" })!;
const device = { v: 1, updater_api: 1, product: "remote-wake", board: "esp32-s3", profile: "hid-power",
  channel: "stable", partition_layout: "rw-4m-ab-v1", version: "1.0.0", release_counter: 1 } as const;
const release = { product: device.product, board: device.board, profile: device.profile, channel: device.channel,
  partition_layout: device.partition_layout, version: "1.1.0", release_counter: 2,
  manifest_url: "https://github.com/example/firmware/releases/download/v1.1/manifest.json" };
let sequence = 0;
const token = "AAAA_ota_token_0123456789abcdefg";

beforeEach(async () => { await caches.default.delete(catalog); });
afterEach(() => { vi.restoreAllMocks(); });
function published(items: unknown[] = [release]): Response {
  return Response.json({ v: 1, releases: items });
}
async function connect(): Promise<{ ws: WebSocket; stub: DurableObjectStub; id: string }> {
  const id = (0xface0000 + ++sequence).toString(16).padStart(16, "0");
  const res = await SELF.fetch(`https://relay.test/v1/device/${id}`, {
    headers: { authorization: `Bearer ${token}`, upgrade: "websocket" },
  });
  const ws = res.webSocket!; ws.accept();
  const ns = (env as Env).DEVICE;
  return { ws, stub: ns.get(ns.idFromName(id)), id };
}
function announcement(ws: WebSocket): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("no update hint")), 3000);
    ws.addEventListener("message", event => {
      const message = JSON.parse(String(event.data)) as { update_available?: unknown };
      if (message.update_available) { clearTimeout(timer); resolve(message.update_available); }
    });
  });
}

describe("bounded release metadata", () => {
  it("requires complete bounded device opt-in and exact updater API", () => {
    expect(registration(device)).toEqual(device);
    for (const bad of [{ ...device, updater_api: 2 }, { ...device, profile: "x".repeat(48) },
      { ...device, channel: "../stable" }, { ...device, release_counter: -1 },
      { ...device, version: "x".repeat(32) }, { ...device, release_counter: 4294967296 }]) {
      expect(registration(bad)).toBeNull();
    }
  });
  it("selects newest counter in the full tuple, never version ordering", () => {
    const all = parseCatalog({ v: 1, releases: [release, { ...release, release_counter: 3, version: "0.1" },
      { ...release, release_counter: 50, partition_layout: "wrong" },
      { ...release, release_counter: 70, product: "voidstats" }] }, source)!;
    expect(matchingRelease(all, device)?.release_counter).toBe(3);
    expect(matchingRelease(all, { ...device, release_counter: 3 })).toBeNull();
  });
  it("accepts only operator-selected HTTPS GitHub sources and versioned assets", () => {
    for (const bad of ["http://127.0.0.1/catalog", "https://localhost/catalog", "https://github.com.evil/x/y",
      "https://github.com/other/repo/releases/download/v1/catalog", "https://token@github.com/example/firmware/releases/download/v1/catalog",
      "https://github.com/example/firmware/releases/download/v1/catalog?token=secret"]) {
      expect(updateSource({ UPDATE_CATALOG_URL: bad, UPDATE_ALLOWED_REPOS: "example/firmware" })).toBeNull();
    }
    for (const bad of ["https://evil.test/firmware", "https://github.com/example/firmware/releases/latest/download/manifest.json",
      "https://raw.githubusercontent.com/example/firmware/main/manifest.json", "https://github.com/other/repo/releases/download/v2/manifest.json"]) {
      expect(parseCatalog({ v: 1, releases: [{ ...release, manifest_url: bad }] }, source)).toBeNull();
    }
    expect(parseCatalog({ v: 1, releases: Array(65).fill(release) }, source)).toBeNull();
    expect(updateSource({})).toBeNull();
  });
  it("caches only validated bounded metadata, follows no redirect and sends no device credentials", async () => {
    const fetcher = vi.spyOn(globalThis, "fetch").mockResolvedValueOnce(published());
    expect(await fetchCatalog(source)).toEqual([release]);
    expect(await fetchCatalog(source)).toEqual([release]);
    expect(fetcher).toHaveBeenCalledTimes(1);
    const [url, options] = fetcher.mock.calls[0]!;
    expect(url).toBe(catalog); expect(options?.redirect).toBe("manual");
    expect(new Headers(options?.headers).has("authorization")).toBe(false);
    await caches.default.delete(catalog);
    fetcher.mockResolvedValueOnce(new Response(null, { status: 302, headers: { location: "http://127.0.0.1/" } }));
    expect(await fetchCatalog(source)).toBeNull();
    fetcher.mockResolvedValueOnce(new Response("x".repeat(MAX_CATALOG_BYTES + 1)));
    expect(await fetchCatalog(source)).toBeNull();
    fetcher.mockResolvedValueOnce(new Response("{}", { headers: { "content-length": String(MAX_CATALOG_BYTES + 1) } }));
    expect(await fetchCatalog(source)).toBeNull();
    fetcher.mockResolvedValueOnce(published([{ ...release, manifest_url: "https://evil.test/bin" }]));
    expect(await fetchCatalog(source)).toBeNull();
  });
});

describe("device announcement transport", () => {
  it("announces on registered connect and refresh, preserving state through hibernation attachments", async () => {
    vi.spyOn(globalThis, "fetch").mockImplementationOnce(async () => published()).mockImplementationOnce(async () => published([
      { ...release, release_counter: 3, manifest_url: "https://github.com/example/firmware/releases/download/v3/manifest.json" },
    ]));
    const { ws, stub } = await connect();
    const first = announcement(ws); ws.send(JSON.stringify({ hello: { fw: "1.0", update: device } }));
    expect(await first).toMatchObject({ v: 1, ...release });
    await evictDurableObject(stub);
    await caches.default.delete(catalog);
    await runInDurableObject(stub, async (_instance, state) => {
      const server = state.getWebSockets()[0]!;
      const data = server.deserializeAttachment() as UpdateAttachment;
      expect(data.registration).toEqual(device);
      data.checked_at = 0; server.serializeAttachment(data);
    });
    const next = announcement(ws);
    await runInDurableObject(stub, async instance => { await (instance as DeviceObject).alarm(); });
    expect(await next).toMatchObject({ release_counter: 3 });
    ws.close();
  });
  it("repeats a still-new matching hint on refresh so busy devices recover", async () => {
    const fetcher = vi.spyOn(globalThis, "fetch").mockImplementation(async () => published());
    const { ws, stub } = await connect();
    const first = announcement(ws);
    ws.send(JSON.stringify({ hello: { update: device } }));
    expect(await first).toMatchObject({ release_counter: 2 });
    await runInDurableObject(stub, async (_instance, state) => {
      const server = state.getWebSockets()[0]!;
      const data = server.deserializeAttachment() as UpdateAttachment;
      data.checked_at = 0; server.serializeAttachment(data);
    });
    const repeated = announcement(ws);
    await runInDurableObject(stub, async instance => { await (instance as DeviceObject).alarm(); });
    expect(await repeated).toMatchObject({ release_counter: 2, manifest_url: release.manifest_url });
    expect(fetcher).toHaveBeenCalledTimes(1); // validated public cache is reused
    ws.close();
  });
  it("rate-bounds repeated registrations and source failures without breaking command transport", async () => {
    const fetcher = vi.spyOn(globalThis, "fetch").mockRejectedValue(new Error("offline"));
    const { ws, stub, id } = await connect();
    await runInDurableObject(stub, async (instance, state) => {
      const server = state.getWebSockets()[0]!;
      const raw = JSON.stringify({ hello: { update: device, catalog_url: "http://127.0.0.1/" } });
      await (instance as DeviceObject).webSocketMessage(server, raw);
      await (instance as DeviceObject).webSocketMessage(server, raw);
      expect((server.deserializeAttachment() as UpdateAttachment).checked_at).toBeGreaterThan(0);
    });
    expect(fetcher).toHaveBeenCalledTimes(1);
    ws.addEventListener("message", event => {
      const command = JSON.parse(String(event.data)) as { id?: string };
      if (command.id) ws.send(JSON.stringify({ id: command.id, ok: true }));
    });
    const response = await SELF.fetch(`https://relay.test/v1/send/${id}`, {
      method: "POST", headers: { authorization: `Bearer ${token}` },
      body: JSON.stringify({ dev: id, id: "after-catalog-failure" }),
    });
    expect(response.status).toBe(200);
    expect(await response.json()).toEqual({ id: "after-catalog-failure", ok: true });
    ws.close();
  });
  it("old hello and invalid capability cause no release fetch", async () => {
    const fetcher = vi.spyOn(globalThis, "fetch");
    const { ws, stub } = await connect();
    await runInDurableObject(stub, async (instance, state) => {
      const server = state.getWebSockets()[0]!;
      await (instance as DeviceObject).webSocketMessage(server, '{"hello":{"fw":"1.0"}}');
      await (instance as DeviceObject).webSocketMessage(server, JSON.stringify({ hello: { update: { ...device, product: [] } } }));
      expect(server.deserializeAttachment()).toBeNull();
    });
    expect(fetcher).not.toHaveBeenCalled();ws.close();
  });
});
