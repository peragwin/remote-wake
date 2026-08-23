/**
 * Token-bucket rate limiter, per device.
 *
 * Defaults: burst of 5, refilled at 10 tokens per minute. A phone can fire five
 * commands back to back and then sustain one every six seconds.
 */

export interface BucketState {
  /** Tokens available at `updated`. Fractional. */
  tokens: number;
  /** Unix milliseconds of the last refill. */
  updated: number;
}

export interface BucketConfig {
  /** Maximum tokens held (the burst size). */
  capacity: number;
  /** Tokens added per minute. */
  refillPerMinute: number;
}

export const DEFAULT_BUCKET: BucketConfig = {
  capacity: 5,
  refillPerMinute: 10,
};

export function newBucket(cfg: BucketConfig, now: number): BucketState {
  return { tokens: cfg.capacity, updated: now };
}

/**
 * Refill according to elapsed time and try to spend one token.
 * Mutates and returns `state`; `allowed` says whether the caller may proceed.
 */
export function consume(
  state: BucketState,
  cfg: BucketConfig,
  now: number,
): { state: BucketState; allowed: boolean } {
  const elapsedMs = Math.max(0, now - state.updated);
  const refilled = (elapsedMs / 60_000) * cfg.refillPerMinute;
  const tokens = Math.min(cfg.capacity, state.tokens + refilled);
  if (tokens >= 1) {
    return { state: { tokens: tokens - 1, updated: now }, allowed: true };
  }
  return { state: { tokens, updated: now }, allowed: false };
}
