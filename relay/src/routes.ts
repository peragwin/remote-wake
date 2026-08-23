/** Path parsing for the v1 relay API. Shared by the Worker and the DO. */

export const DEVICE_ID_RE = /^[0-9a-f]{16}$/;

export type RouteKind = "device" | "send" | "presence";

export interface Route {
  kind: RouteKind;
  deviceId: string;
}

const KINDS: Record<string, RouteKind> = {
  device: "device",
  send: "send",
  presence: "presence",
};

/**
 * `/v1/{device,send,presence}/<deviceId>` → route, or `null` if the path is
 * not a v1 route. Returns `"bad_device_id"` when the shape is right but the
 * device id is not 16 lowercase hex characters.
 */
export function parseRoute(pathname: string): Route | null | "bad_device_id" {
  const parts = pathname.split("/").filter((p) => p.length > 0);
  if (parts.length !== 3 || parts[0] !== "v1") return null;
  const kind = KINDS[parts[1]!];
  if (!kind) return null;
  const deviceId = parts[2]!;
  if (!DEVICE_ID_RE.test(deviceId)) return "bad_device_id";
  return { kind, deviceId };
}
