/**
 * Token hashing + constant-time comparison.
 *
 * The relay never stores a device token, only SHA-256(token) as lowercase hex.
 * That is enough to authenticate later presenters and useless for impersonating
 * the device anywhere else.
 */

const encoder = new TextEncoder();

/** SHA-256 of a UTF-8 string, lowercase hex. */
export async function sha256Hex(input: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", encoder.encode(input));
  const bytes = new Uint8Array(digest);
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
}

interface TimingSafeSubtle {
  timingSafeEqual?: (a: ArrayBufferView, b: ArrayBufferView) => boolean;
}

/**
 * Constant-time equality for two hex digests.
 *
 * Uses the Workers runtime's `crypto.subtle.timingSafeEqual` when present and
 * falls back to an XOR-accumulate loop that does not short-circuit. Both
 * operands here are fixed-length hex digests, so length is not a secret.
 */
export function constantTimeEqualHex(a: string, b: string): boolean {
  const av = encoder.encode(a);
  const bv = encoder.encode(b);
  if (av.length !== bv.length) return false;

  const subtle = crypto.subtle as unknown as TimingSafeSubtle;
  if (typeof subtle.timingSafeEqual === "function") {
    return subtle.timingSafeEqual(av, bv);
  }

  let diff = 0;
  for (let i = 0; i < av.length; i++) diff |= av[i]! ^ bv[i]!;
  return diff === 0;
}
