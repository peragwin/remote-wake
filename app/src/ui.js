/** UI primitives: toasts, bottom sheets, and the press-and-hold controller. */

import { $, el, haptic, prefersReducedMotion } from './util.js';

/* ─────────────────────────────────────────────────────────── toasts ── */

const ICONS = {
  ok: 'M20 6 9 17l-5-5',
  err: 'M12 8v5M12 16.5v.5M10.3 4 3 17a2 2 0 0 0 1.7 3h14.6A2 2 0 0 0 21 17L13.7 4a2 2 0 0 0-3.4 0Z',
  info: 'M12 16v-5M12 8v.5M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0Z',
};

let toastSeq = 0;

/**
 * @param {'ok'|'err'|'info'} kind
 * @param {string} title
 * @param {string} [detail]
 * @param {number} [ms] auto-dismiss delay
 */
export function toast(kind, title, detail = '', ms = kind === 'err' ? 6000 : 3400) {
  const host = $('#toasts');
  if (!host) return;

  const id = ++toastSeq;
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('class', 'toast-ico');
  svg.setAttribute('aria-hidden', 'true');
  const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  path.setAttribute('d', ICONS[kind] || ICONS.info);
  svg.append(path);

  const node = el(
    'div',
    { class: `toast toast-${kind}`, 'data-id': id },
    svg,
    el(
      'div',
      { class: 'toast-body' },
      el('div', { class: 'toast-title', text: title }),
      detail ? el('div', { class: 'toast-detail', text: detail }) : null
    )
  );

  node.addEventListener('click', () => dismiss(node));
  host.append(node);

  // Never stack more than three.
  while (host.children.length > 3) dismiss(host.firstElementChild, true);

  const timer = setTimeout(() => dismiss(node), ms);
  node._timer = timer;
  return node;
}

function dismiss(node, immediate = false) {
  if (!node || node._leaving) return;
  node._leaving = true;
  clearTimeout(node._timer);
  if (immediate || prefersReducedMotion()) {
    node.remove();
    return;
  }
  node.classList.add('leaving');
  setTimeout(() => node.remove(), 220);
}

/* ─────────────────────────────────────────────────────────── sheets ── */

let sheetResolve = null;

function closeSheet(value) {
  const sheet = $('#sheet');
  const scrim = $('#sheet-scrim');
  sheet.hidden = true;
  scrim.hidden = true;
  $('#sheet-extra').replaceChildren();
  $('#sheet-actions').replaceChildren();
  document.removeEventListener('keydown', onSheetKey);
  const resolve = sheetResolve;
  sheetResolve = null;
  resolve?.(value);
}

function onSheetKey(e) {
  if (e.key === 'Escape') closeSheet(null);
}

/**
 * Modal bottom sheet.
 * @param {{title:string, body?:string, extra?:Node, actions:Array<{label:string,
 *          value:any, style?:'primary'|'danger'|'ghost'}>}} opts
 * @returns {Promise<any>} the chosen action's `value`, or null if dismissed
 */
export function sheet({ title, body = '', extra = null, actions = [] }) {
  if (sheetResolve) closeSheet(null);

  $('#sheet-title').textContent = title;
  $('#sheet-body').textContent = body;
  const extraHost = $('#sheet-extra');
  extraHost.replaceChildren();
  if (extra) extraHost.append(extra);

  const actionHost = $('#sheet-actions');
  actionHost.replaceChildren();
  for (const a of actions) {
    const cls =
      a.style === 'danger' ? 'btn btn-danger' : a.style === 'primary' ? 'btn btn-primary' : 'btn btn-ghost';
    actionHost.append(
      el('button', { class: `${cls} btn-block`, type: 'button', text: a.label, onclick: () => closeSheet(a.value) })
    );
  }

  $('#sheet-scrim').hidden = false;
  $('#sheet').hidden = false;
  $('#sheet-scrim').onclick = () => closeSheet(null);
  document.addEventListener('keydown', onSheetKey);
  actionHost.querySelector('button')?.focus({ preventScroll: true });

  return new Promise((resolve) => {
    sheetResolve = resolve;
  });
}

/** Two-step confirmation for destructive actions. */
export function confirmDestructive({ title, body, confirmLabel = 'Confirm' }) {
  return sheet({
    title,
    body,
    actions: [
      { label: confirmLabel, value: true, style: 'danger' },
      { label: 'Cancel', value: false },
    ],
  }).then((v) => v === true);
}

/* ──────────────────────────────────────────────── press-and-hold ──── */

/**
 * Wire a button so that it only fires after the user physically holds it for
 * `durationMs`, showing a progress ring the whole time. Releasing early aborts.
 *
 * The ring is a genuine progress indicator, not decoration, so it keeps
 * animating under prefers-reduced-motion — it is driven by rAF setting
 * stroke-dashoffset directly, with no CSS transition involved.
 *
 * @param {HTMLElement} button must contain `.hold-progress`
 * @param {{durationMs?:number, onComplete:Function, onStart?:Function, onCancel?:Function}} opts
 * @returns {{destroy:Function}}
 */
export function attachHold(button, { durationMs = 1500, onComplete, onStart, onCancel }) {
  const ring = button.querySelector('.hold-progress');
  const CIRCUMFERENCE = 283; // 2π × r(45), matches the SVG in index.html
  let raf = 0;
  let startedAt = 0;
  let active = false;

  const setProgress = (p) => {
    if (ring) ring.style.strokeDashoffset = String(CIRCUMFERENCE * (1 - p));
  };

  function frame(now) {
    if (!active) return;
    const p = Math.min(1, (now - startedAt) / durationMs);
    setProgress(p);
    if (p >= 1) {
      const finish = onComplete;
      stop(false);
      haptic(28);
      finish?.();
      return;
    }
    raf = requestAnimationFrame(frame);
  }

  function start(e) {
    if (active || button.disabled) return;
    // Only primary pointer; ignore right-click and multi-touch.
    if (e.button !== undefined && e.button !== 0) return;
    e.preventDefault();
    active = true;
    startedAt = performance.now();
    button.classList.add('arming');
    button.setPointerCapture?.(e.pointerId);
    haptic(10);
    onStart?.();
    raf = requestAnimationFrame(frame);
  }

  function stop(cancelled = true) {
    if (!active) return;
    active = false;
    cancelAnimationFrame(raf);
    button.classList.remove('arming');
    setProgress(0);
    if (cancelled) onCancel?.();
  }

  const onUp = () => stop(true);

  button.addEventListener('pointerdown', start);
  button.addEventListener('pointerup', onUp);
  button.addEventListener('pointercancel', onUp);
  button.addEventListener('pointerleave', onUp);
  // Keyboard parity: Space/Enter holds while held down.
  button.addEventListener('keydown', (e) => {
    if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) start({ preventDefault: () => e.preventDefault() });
  });
  button.addEventListener('keyup', onUp);
  button.addEventListener('blur', onUp);
  button.addEventListener('contextmenu', (e) => e.preventDefault());

  setProgress(0);
  return { destroy: () => stop(true) };
}

/* ───────────────────────────────────────────────────────── helpers ── */

/** Clipboard with a graceful fallback for non-secure contexts. */
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    try {
      const ta = el('textarea', { class: 'mono' });
      ta.value = text;
      ta.style.cssText = 'position:fixed;opacity:0;pointer-events:none';
      document.body.append(ta);
      ta.select();
      const ok = document.execCommand?.('copy');
      ta.remove();
      return !!ok;
    } catch {
      return false;
    }
  }
}

/** Momentary state class on an element (for the WAKE button feedback). */
export function flash(node, className, ms = 700) {
  if (!node) return;
  node.classList.add(className);
  setTimeout(() => node.classList.remove(className), ms);
}
