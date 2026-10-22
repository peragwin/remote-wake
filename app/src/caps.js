/**
 * Device capabilities: which actions the device's firmware accepts.
 *
 * Firmware that reports them puts an `acts` list in its `status` reply
 * (docs/protocol.md §Actions); `type` and `keys` are a build option that is off
 * by default. Firmware without the list accepts every action, so a missing list
 * keeps the old behaviour: everything enabled.
 *
 * This only shapes the UI. The device enforces the list itself, and the relay
 * could forge an unsigned `status` reply either way.
 *
 * Dependency-free so it can be unit tested outside a browser.
 */

const MAX_ACTS = 32;
const MAX_ACT_LEN = 24; // RW_ACT_MAXLEN in firmware/main/auth.h

/**
 * The `acts` list from a `status` result, or null when the firmware sent none
 * (or sent something that is not a list).
 * @returns {string[]|null}
 */
export function parseActs(res) {
  const acts = res?.acts;
  if (!Array.isArray(acts)) return null;
  const out = [];
  for (const a of acts.slice(0, MAX_ACTS)) {
    if (typeof a === 'string' && a && a.length < MAX_ACT_LEN && !out.includes(a)) out.push(a);
  }
  return out;
}

/** Whether to offer @p act, given a stored list (null/undefined: unknown → yes). */
export function actAllowed(acts, act) {
  return !Array.isArray(acts) || acts.includes(act);
}

/** Order-insensitive equality, treating null and undefined alike. */
export function sameActs(a, b) {
  if (!Array.isArray(a) || !Array.isArray(b)) return !Array.isArray(a) && !Array.isArray(b);
  return a.length === b.length && a.every((x) => b.includes(x));
}
