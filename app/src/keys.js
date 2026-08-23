/**
 * Key names for the `keys` action.
 *
 * Names are passed through to the firmware's US-layout HID mapping. The app
 * deliberately does not keep an allow-list: the composer accepts any
 * /^[A-Z0-9_]+$/ token, so a newer firmware's key names work without an app
 * update, and the device is the one authority on what it can press.
 */

/** Pretty labels for the chips and sequence strip. */
const PRETTY = {
  LCTRL: 'Ctrl', RCTRL: 'R-Ctrl',
  LSHIFT: 'Shift', RSHIFT: 'R-Shift',
  LALT: 'Alt', RALT: 'AltGr',
  LGUI: 'Win', RGUI: 'R-Win',
  ENTER: 'Enter', ESC: 'Esc', BACKSPACE: '⌫', TAB: 'Tab', SPACE: 'Space',
  DELETE: 'Del', INSERT: 'Ins', PAGEUP: 'PgUp', PAGEDOWN: 'PgDn',
  UP: '↑', DOWN: '↓', LEFT: '←', RIGHT: '→',
};

export const prettyKey = (k) => PRETTY[k] || k;
export const prettyChord = (chord) => chord.map(prettyKey).join(' + ');

/** One-tap presets. `seq` is exactly what goes into args.seq. */
export const PRESETS = [
  { id: 'ctrl-alt-del', label: 'Ctrl + Alt + Del', seq: [['LCTRL', 'LALT', 'DELETE']] },
  { id: 'win-l', label: 'Win + L', seq: [['LGUI', 'L']] },
  { id: 'enter', label: 'Enter', seq: [['ENTER']] },
  { id: 'esc', label: 'Esc', seq: [['ESC']] },
  { id: 'space', label: 'Space', seq: [['SPACE']] },
  { id: 'alt-tab', label: 'Alt + Tab', seq: [['LALT', 'TAB']] },
  { id: 'ctrl-shift-esc', label: 'Ctrl + Shift + Esc', seq: [['LCTRL', 'LSHIFT', 'ESC']] },
  { id: 'win-r', label: 'Win + R', seq: [['LGUI', 'R']] },
];

export const isValidKeyName = (k) => typeof k === 'string' && /^[A-Z0-9_]{1,16}$/.test(k);

/** Parse "ctrl+alt+del" / "LGUI+L" into a chord of canonical key names. */
export function parseChord(input) {
  const aliases = {
    CTRL: 'LCTRL', CONTROL: 'LCTRL', SHIFT: 'LSHIFT', ALT: 'LALT',
    WIN: 'LGUI', WINDOWS: 'LGUI', SUPER: 'LGUI', CMD: 'LGUI', META: 'LGUI',
    DEL: 'DELETE', RETURN: 'ENTER', ESCAPE: 'ESC', PGUP: 'PAGEUP', PGDN: 'PAGEDOWN',
    BKSP: 'BACKSPACE', SPACEBAR: 'SPACE',
  };
  const parts = String(input)
    .toUpperCase()
    .split(/[+\s,]+/)
    .map((p) => p.trim())
    .filter(Boolean)
    .map((p) => aliases[p] || p);

  if (!parts.length) throw new Error('Type a key combo, e.g. Ctrl+Alt+F4');
  if (parts.length > 6) throw new Error('Too many keys in one chord.');
  for (const p of parts) {
    if (!isValidKeyName(p)) throw new Error(`Not a key name: ${p}`);
  }
  return [...new Set(parts)];
}
