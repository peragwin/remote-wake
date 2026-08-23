/**
 * remote-wake relay — Cloudflare Worker entrypoint.
 *
 * The Worker is a pure router: it validates the path shape and the presence of
 * a bearer credential, then hands the request to the device's Durable Object,
 * which owns authentication, the websocket, and all limits.
 *
 * The relay is deliberately zero-knowledge (docs/security.md): it never
 * inspects `sig`, `ctr`, `act` or `args`, never persists a payload, and never
 * logs a body.
 */

import { bearerToken, errJson } from "./http";
import { parseRoute } from "./routes";
import type { Env } from "./types";

export { DeviceObject } from "./device";

const METHODS: Record<string, string> = {
  device: "GET",
  send: "POST",
  presence: "GET",
};

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const route = parseRoute(url.pathname);

    if (route === null) return errJson(404, "not_found");
    if (route === "bad_device_id") return errJson(404, "bad_device_id");

    if (request.method !== METHODS[route.kind]) {
      return errJson(405, "method_not_allowed");
    }

    // Auth is re-checked inside the DO against the claimed token hash; this is
    // only a cheap edge rejection of unauthenticated traffic.
    if (bearerToken(request) === null) return errJson(401, "unauthorized");

    const id = env.DEVICE.idFromName(route.deviceId);
    return env.DEVICE.get(id).fetch(request);
  },
} satisfies ExportedHandler<Env>;
