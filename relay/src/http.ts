/**
 * Small JSON/HTTP helpers shared by the Worker router and the Durable Object.
 *
 * Every error the relay emits is `{"err": "<code>"}` so that the phone and the
 * device only ever have to parse one shape.
 */

const JSON_HEADERS = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store",
} as const;

/** `{"err": code}` with the given status. */
export function errJson(status: number, code: string): Response {
  return new Response(JSON.stringify({ err: code }), {
    status,
    headers: JSON_HEADERS,
  });
}

/** 200 with an already-serialized JSON body (forwarded verbatim). */
export function rawJson(body: string, status = 200): Response {
  return new Response(body, { status, headers: JSON_HEADERS });
}

/** 200 with a structure we produced ourselves. */
export function okJson(value: unknown, status = 200): Response {
  return new Response(JSON.stringify(value), { status, headers: JSON_HEADERS });
}

/**
 * CORS for the browser-hosted PWA. Bearer auth only (no cookies), so a
 * wildcard origin grants nothing beyond what any non-browser client has.
 */
export const CORS_HEADERS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, POST, OPTIONS",
  "access-control-allow-headers": "authorization, content-type",
  "access-control-max-age": "86400",
} as const;

/** Re-wrap a response with CORS headers appended (DO responses are immutable). */
export function withCors(res: Response): Response {
  const out = new Response(res.body, res);
  for (const [k, v] of Object.entries(CORS_HEADERS)) out.headers.set(k, v);
  return out;
}

/**
 * Extract the bearer credential from an `Authorization` header.
 * Returns `null` when the header is missing, malformed, or empty.
 */
export function bearerToken(req: Request): string | null {
  const header = req.headers.get("authorization");
  if (!header) return null;
  const space = header.indexOf(" ");
  if (space < 0) return null;
  if (header.slice(0, space).toLowerCase() !== "bearer") return null;
  const token = header.slice(space + 1).trim();
  return token.length > 0 ? token : null;
}
