// Stand-in for src/ui.js inside the app harness: records toasts and dialogs and
// answers dialogs through the active harness (see app_harness.mjs).
const h = () => globalThis.__rwHarness;

export function toast(kind, title, detail = '') {
  h().toasts.push({ kind, title, detail });
}
export function sheet(opts) {
  h().dialogs.push({ kind: 'sheet', ...opts });
  return Promise.resolve(h().answer('sheet', opts));
}
export function confirmDestructive(opts) {
  h().dialogs.push({ kind: 'confirm', ...opts });
  return Promise.resolve(h().answer('confirm', opts));
}
export function attachHold() {}
export async function copyText() {
  return true;
}
export function flash() {}
