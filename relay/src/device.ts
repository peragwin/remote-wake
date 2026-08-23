import { DurableObject } from "cloudflare:workers";

import { constantTimeEqualHex, sha256Hex } from "./crypto";
import { bearerToken, errJson, okJson, rawJson } from "./http";
import {
  DEFAULT_BUCKET,
  consume,
  newBucket,
  type BucketState,
} from "./ratelimit";
import { parseRoute } from "./routes";
import type { Env } from "./types";

/** Maximum accepted `POST /v1/send` body (protocol: 4 KiB). */
export const MAX_SEND_BYTES = 4096;
/** Maximum accepted device → relay frame. Larger frames are dropped. */
export const MAX_DEVICE_FRAME_BYTES = 16 * 1024;
/** How long `POST /v1/send` long-polls for the device's response. */
export const RESPONSE_TIMEOUT_MS = 10_000;
/** Concurrent in-flight commands per device. */
export const PENDING_CAP = 8;
/** Server-initiated keepalive interval. */
export const PING_INTERVAL_MS = 25_000;
/** WS close code used when a newer device socket supersedes an older one. */
export const CLOSE_SUPERSEDED = 4001;

const K_TOKEN_HASH = "th";
const K_SINCE = "since";
const K_BUCKET = "rl";
const K_GEN = "gen";

/** Tag identifying the current device socket generation. */
const genTag = (gen: number) => `g${gen}`;

interface Pending {
  resolve: (raw: string) => void;
}

/**
 * Read at most `max` bytes of the request body. Returns `null` (after draining
 * the rest of the stream, so the connection closes cleanly) if the body is
 * larger, without ever accumulating the overflow.
 */
async function readBounded(
  request: Request,
  max: number,
): Promise<Uint8Array | null> {
  const body = request.body;
  if (!body) return new Uint8Array(0);

  const reader = body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  let overflow = false;

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (!value) continue;
    total += value.byteLength;
    if (total > max) {
      overflow = true;
      chunks.length = 0;
      continue; // Keep draining, keep nothing.
    }
    chunks.push(value);
  }
  if (overflow) return null;

  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return out;
}

/**
 * One Durable Object per deviceId.
 *
 * Holds exactly three pieces of state: the SHA-256 of the claiming token, a
 * presence timestamp, and a rate-limit bucket. Command and response payloads
 * pass through memory only — nothing is written to storage and nothing is
 * logged.
 */
export class DeviceObject extends DurableObject<Env> {
  /** SHA-256(deviceToken) hex, or null while the device is unclaimed. */
  private tokenHash: string | null = null;
  /** Unix ms of the last online/offline transition. */
  private since = 0;
  private bucket: BucketState;
  /**
   * Socket generation. Superseded sockets can linger in `getWebSockets()`
   * while their close handshake completes, so the live one is identified by
   * its tag rather than by position.
   */
  private gen = 0;
  /** Command id → resolver for the in-flight long-poll. Memory only. */
  private readonly pending = new Map<string, Pending>();

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.bucket = newBucket(DEFAULT_BUCKET, Date.now());
    ctx.blockConcurrencyWhile(async () => {
      const [hash, since, bucket, gen] = await Promise.all([
        ctx.storage.get<string>(K_TOKEN_HASH),
        ctx.storage.get<number>(K_SINCE),
        ctx.storage.get<BucketState>(K_BUCKET),
        ctx.storage.get<number>(K_GEN),
      ]);
      this.tokenHash = hash ?? null;
      this.since = since ?? 0;
      this.gen = gen ?? 0;
      if (bucket) this.bucket = bucket;
      // Hibernation wake-up: the auto-response pair is per-instance state.
      if (ctx.getWebSockets().length > 0) this.armAutoResponse();
    });
  }

  /** The one socket that currently represents the device, if any. */
  private activeSocket(): WebSocket | null {
    if (this.gen === 0) return null;
    const sockets = this.ctx.getWebSockets(genTag(this.gen));
    for (const ws of sockets) {
      if (ws.readyState === WebSocket.READY_STATE_OPEN) return ws;
    }
    return null;
  }

  // ---------------------------------------------------------------- routing

  override async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const route = parseRoute(url.pathname);
    if (!route || route === "bad_device_id") return errJson(404, "not_found");

    const token = bearerToken(request);
    if (!token) return errJson(401, "unauthorized");
    if (!(await this.authorize(token))) return errJson(403, "forbidden");

    switch (route.kind) {
      case "device":
        return this.handleUpgrade(request);
      case "send":
        return this.handleSend(request, route.deviceId);
      case "presence":
        return this.handlePresence();
    }
  }

  // ------------------------------------------------------------------- auth

  /**
   * Trust on first use: the first token ever presented for this deviceId
   * claims it. Every later request must present a token with the same digest.
   *
   * The compare-and-claim below is synchronous after the digest, so two
   * concurrent first requests cannot both claim.
   */
  private async authorize(token: string): Promise<boolean> {
    const hash = await sha256Hex(token);
    if (this.tokenHash === null) {
      this.tokenHash = hash;
      await this.ctx.storage.put(K_TOKEN_HASH, hash);
      return true;
    }
    return constantTimeEqualHex(this.tokenHash, hash);
  }

  // -------------------------------------------------------------- websocket

  private async handleUpgrade(request: Request): Promise<Response> {
    if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return errJson(426, "upgrade_required");
    }

    // One device socket at a time — the newcomer wins.
    for (const old of this.ctx.getWebSockets()) {
      try {
        old.close(CLOSE_SUPERSEDED, "superseded");
      } catch {
        /* already gone */
      }
    }

    this.gen += 1;
    await this.ctx.storage.put(K_GEN, this.gen);

    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];

    // Hibernatable: the DO burns no duration while the socket is idle.
    this.ctx.acceptWebSocket(server, [genTag(this.gen)]);
    this.armAutoResponse();

    this.since = Date.now();
    void this.ctx.storage.put(K_SINCE, this.since);
    void this.ctx.storage.setAlarm(Date.now() + PING_INTERVAL_MS);

    return new Response(null, { status: 101, webSocket: client });
  }

  /** Answer device-initiated `ping` frames without waking the object. */
  private armAutoResponse(): void {
    this.ctx.setWebSocketAutoResponse(
      new WebSocketRequestResponsePair("ping", "pong"),
    );
  }

  override webSocketMessage(_ws: WebSocket, message: string | ArrayBuffer) {
    // Binary frames are not part of the protocol.
    if (typeof message !== "string") return;
    if (message.length > MAX_DEVICE_FRAME_BYTES) return;

    let parsed: unknown;
    try {
      parsed = JSON.parse(message);
    } catch {
      return; // Well-formedness is the only thing we check.
    }
    if (typeof parsed !== "object" || parsed === null) return;

    const id = (parsed as { id?: unknown }).id;
    if (typeof id !== "string") return; // e.g. the `hello` frame.

    const waiter = this.pending.get(id);
    if (!waiter) return; // Unsolicited or already timed out.
    this.pending.delete(id);
    waiter.resolve(message); // Forwarded verbatim; never inspected further.
  }

  override webSocketClose() {
    // The runtime has already removed the socket; never call close() here —
    // a throw inside a hibernation handler resets the whole object.
    this.markPresence();
  }

  override webSocketError() {
    this.markPresence();
  }

  /** Force-drop a socket we found to be dead while sending. */
  private onSocketGone(ws: WebSocket): void {
    try {
      ws.close(1011, "send failed");
    } catch {
      /* already closed */
    }
    this.markPresence();
  }

  private markPresence(): void {
    if (this.activeSocket() === null) {
      this.since = Date.now();
      void this.ctx.storage.put(K_SINCE, this.since);
      void this.ctx.storage.deleteAlarm();
    }
  }

  /** Server-side keepalive every 25 s (protocol Transport section). */
  override async alarm(): Promise<void> {
    const ws = this.activeSocket();
    if (!ws) return;

    const frame = JSON.stringify({ v: 1, ping: Math.floor(Date.now() / 1000) });
    try {
      ws.send(frame);
    } catch {
      this.onSocketGone(ws);
      return;
    }
    await this.ctx.storage.setAlarm(Date.now() + PING_INTERVAL_MS);
  }

  // ------------------------------------------------------------------- send

  private async handleSend(
    request: Request,
    deviceId: string,
  ): Promise<Response> {
    if (request.method !== "POST") return errJson(405, "method_not_allowed");

    // The body is read (never streamed past the cap) so that an oversized
    // request is rejected without the relay ever holding more than one chunk
    // beyond the limit.
    const buf = await readBounded(request, MAX_SEND_BYTES);
    if (buf === null) return errJson(413, "too_large");

    const raw = new TextDecoder().decode(buf);

    let envelope: unknown;
    try {
      envelope = JSON.parse(raw);
    } catch {
      return errJson(400, "bad_json");
    }
    if (typeof envelope !== "object" || envelope === null || Array.isArray(envelope)) {
      return errJson(400, "bad_envelope");
    }

    // The only two fields the relay is allowed to look at.
    const { dev, id } = envelope as { dev?: unknown; id?: unknown };
    if (dev !== deviceId) return errJson(400, "dev_mismatch");
    if (typeof id !== "string" || id.length === 0 || id.length > 128) {
      return errJson(400, "bad_envelope");
    }

    const now = Date.now();
    const result = consume(this.bucket, DEFAULT_BUCKET, now);
    this.bucket = result.state;
    void this.ctx.storage.put(K_BUCKET, this.bucket);
    if (!result.allowed) return errJson(429, "rate_limited");

    const ws = this.activeSocket();
    if (!ws) return errJson(504, "device_offline");

    if (this.pending.size >= PENDING_CAP) return errJson(429, "too_many_pending");
    if (this.pending.has(id)) return errJson(409, "duplicate_id");

    let settle!: (raw: string | null) => void;
    const answer = new Promise<string | null>((resolve) => {
      settle = resolve;
    });
    this.pending.set(id, { resolve: (r) => settle(r) });

    const timer = setTimeout(() => {
      if (this.pending.delete(id)) settle(null);
    }, RESPONSE_TIMEOUT_MS);

    try {
      ws.send(raw); // Verbatim: the signature covers these exact bytes.
    } catch {
      clearTimeout(timer);
      this.pending.delete(id);
      this.onSocketGone(ws);
      return errJson(504, "device_offline");
    }

    const response = await answer;
    clearTimeout(timer);
    if (response === null) return errJson(504, "device_timeout");
    return rawJson(response);
  }

  // --------------------------------------------------------------- presence

  private handlePresence(): Response {
    const online = this.activeSocket() !== null;
    return okJson({
      online,
      since: this.since === 0 ? null : Math.floor(this.since / 1000),
    });
  }
}
