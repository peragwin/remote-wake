/**
 * remote-wake PWA — application controller.
 *
 * Wires the three views (pair / home / settings) to the modules that hold the
 * actual contract: signing.js, crypto.js, commands.js, api.js.
 */

import { $, $$, el, since, haptic, setHapticsEnabled, uuid4, nowSec } from './util.js';
import * as store from './store.js';
import * as crypto_ from './crypto.js';
import * as webauthn from './webauthn.js';
import * as cmd from './commands.js';
import { createPresencePoller, errorMessage, RelayError } from './api.js';
import { toast, sheet, confirmDestructive, attachHold, copyText, flash } from './ui.js';
import { PRESETS, prettyChord, parseChord } from './keys.js';
import { toSVG } from './qr.js';
import { parsePairingBlob } from './pairing.js';

const state = {
  device: null,
  key: null,
  settings: null,
  presence: { online: false },
  sequence: [], // array of chords for the `keys` composer
  busy: false,
  view: 'home',
};

let poller = null;

/* ═══════════════════════════════════════════════════════════ boot ══ */

async function boot() {
  try {
    state.settings = await store.getSettings();
    setHapticsEnabled(state.settings.hapticsEnabled);
    state.device = await store.getActiveDevice();
    state.key = (await crypto_.getKeyInfo()) || (await crypto_.ensureKey());
  } catch (err) {
    console.error('boot failed', err);
    $('#boot').innerHTML =
      '<p class="boot-text">Could not open local storage.<br>Private browsing can block IndexedDB.</p>';
    return;
  }

  bindGlobal();
  bindHome();
  bindPair();
  bindSettings();

  showView(state.device ? 'home' : 'pair');

  $('#topbar').hidden = false;
  $('#app').hidden = false;
  $('#boot').hidden = true;

  poller = createPresencePoller({
    getDevice: () => state.device,
    onUpdate: renderPresence,
    onError: (err) => console.debug('presence', err.code),
  });
  if (state.device) poller.start();

  handleLaunchIntent();
  registerServiceWorker();
}

/**
 * The manifest shortcut opens ./index.html?action=wake. It deliberately does
 * NOT fire the command: signing needs a real user gesture (WebAuthn requires
 * one) and nothing should actuate a PC because an icon was long-pressed. It
 * just brings the button under the thumb and draws the eye to it.
 */
function handleLaunchIntent() {
  const action = new URLSearchParams(location.search).get('action');
  if (action !== 'wake' || !state.device) return;
  const btn = $('#btn-wake');
  btn.focus({ preventScroll: true });
  flash(btn, 'sending', 1600);
  history.replaceState(null, '', location.pathname);
}

function registerServiceWorker() {
  if (!('serviceWorker' in navigator)) return;
  if (location.protocol !== 'https:' && location.hostname !== 'localhost') return;
  navigator.serviceWorker.register('./sw.js', { scope: './' }).catch((err) => {
    console.debug('service worker registration skipped:', err.message);
  });
}

/* ═══════════════════════════════════════════════════════════ views ══ */

function showView(name) {
  state.view = name;
  for (const v of $$('.view')) v.hidden = v.id !== `view-${name}`;
  $('#btn-settings').hidden = name === 'settings';
  window.scrollTo({ top: 0, behavior: 'instant' });

  if (name === 'home') renderHome();
  if (name === 'settings') renderSettings();
  if (name === 'pair') renderPair();
}

function bindGlobal() {
  $('#btn-settings').addEventListener('click', () => showView('settings'));
  $('#btn-back').addEventListener('click', () => showView(state.device ? 'home' : 'pair'));
  $('#build-note').textContent = state.key ? state.key.publicKeyB64url.slice(0, 8) : '';
}

/* ══════════════════════════════════════════════════════ home view ══ */

function bindHome() {
  $('#btn-wake').addEventListener('click', onWake);
  $('#btn-refresh').addEventListener('click', onRefresh);
  $('#btn-unlock').addEventListener('click', onUnlock);
  $('#btn-unlock-edit').addEventListener('click', () => showView('settings'));
  $('#btn-power-tap').addEventListener('click', onPowerTap);
  $('#btn-keys-send').addEventListener('click', onSendKeys);
  $('#btn-keys-clear').addEventListener('click', () => {
    state.sequence = [];
    renderSequence();
  });

  renderChips();

  attachHold($('#btn-power-hold'), {
    durationMs: 1500,
    onComplete: onPowerHoldArmed,
  });
}

function renderHome() {
  const d = state.device;
  if (!d) return;
  $('#device-name').textContent = d.name || 'Device';
  $('#device-id').textContent = d.deviceId;
  renderUnlockCard();
  renderSequence();
}

function renderPresence(p) {
  state.presence = p || { online: false };
  const dot = $('#presence-dot');
  const text = $('#presence-text');
  if (!p) {
    dot.dataset.state = 'unknown';
    text.textContent = 'No device paired';
    return;
  }
  if (p.error) {
    dot.dataset.state = 'error';
    text.textContent = p.error === 'unauthorized' ? 'Relay rejected the token' : 'Relay unreachable';
    return;
  }
  dot.dataset.state = p.online ? 'online' : 'offline';
  text.textContent = p.online
    ? p.since
      ? `Online · connected ${since(p.since)} ago`
      : 'Online'
    : 'Offline · device is not connected to the relay';
}

async function onRefresh() {
  const btn = $('#btn-refresh');
  if (btn.classList.contains('spinning')) return;
  btn.classList.add('spinning');
  try {
    await poller?.refresh();
    // `status` is read-only; it is the one action that skips the presence gate
    // so that a routine refresh does not demand a fingerprint.
    const r = await cmd.status(state.device);
    if (r.ok) {
      renderStats(r.res || {});
      toast('ok', 'Status updated');
    } else {
      toast('err', 'Status failed', errorMessage(r.err, r.res));
    }
  } catch (err) {
    handleError(err, 'Status failed');
  } finally {
    btn.classList.remove('spinning');
  }
}

function renderStats(res) {
  const rssi = res.rssi ?? res.wifiRssi;
  $('#stat-rssi').textContent = Number.isFinite(rssi) ? `${rssi} dBm` : '—';
  const usb = res.usb ?? (res.usbMounted === undefined ? undefined : res.usbMounted ? 'mounted' : 'down');
  $('#stat-usb').textContent =
    usb === undefined ? '—' : res.usbSuspended ? 'suspended' : String(usb);
  $('#stat-fw').textContent = res.fw || res.version || '—';
}

async function onWake() {
  const btn = $('#btn-wake');
  if (state.busy) return;
  btn.classList.add('sending');
  await guard(
    async () => {
      const r = await cmd.wake(state.device);
      if (r.ok) {
        flash(btn, 'ok');
        toast('ok', 'Wake sent', 'Left-Ctrl tapped twice.');
      } else {
        flash(btn, 'err', 500);
        toast('err', 'Rejected', errorMessage(r.err, r.res));
      }
    },
    'Wake failed',
    () => flash(btn, 'err', 500)
  );
  btn.classList.remove('sending');
}

/* ── unlock ───────────────────────────────────────────────────────── */

function renderUnlockCard() {
  const hasText = !!state.device?.unlockText;
  $('#btn-unlock').disabled = !hasText;
  $('#unlock-sub').textContent = hasText
    ? `Types ${state.device.unlockText.length} characters${
        state.device.unlockEnter === false ? '' : ', then Enter'
      }. Stored on this phone only.`
    : 'No unlock text stored. Add one in Settings.';
}

async function onUnlock() {
  const text = state.device?.unlockText;
  if (!text) return;
  await guard(async () => {
    const r = await cmd.typeText(state.device, text, state.device.unlockEnter !== false);
    if (r.ok) toast('ok', 'Unlock sent');
    else toast('err', 'Rejected', errorMessage(r.err, r.res));
  }, 'Unlock failed');
}

/* ── key chords ───────────────────────────────────────────────────── */

function renderChips() {
  const row = $('#chip-row');
  row.replaceChildren(
    ...PRESETS.map((p) =>
      el('button', {
        class: 'chip',
        type: 'button',
        text: p.label,
        onclick: () => {
          state.sequence.push(...p.seq);
          haptic();
          renderSequence();
        },
      })
    ),
    el('button', { class: 'chip chip-add', type: 'button', text: '+ Custom', onclick: onCustomChord })
  );
}

function renderSequence() {
  const strip = $('#seq-strip');
  const list = $('#seq-list');
  strip.hidden = state.sequence.length === 0;
  $('#btn-keys-clear').hidden = state.sequence.length === 0;

  list.replaceChildren();
  state.sequence.forEach((chord, i) => {
    if (i) list.append(el('span', { class: 'seq-arrow', text: '›' }));
    list.append(
      el(
        'span',
        { class: 'seq-item' },
        prettyChord(chord),
        el('button', {
          type: 'button',
          'aria-label': `Remove ${prettyChord(chord)}`,
          text: '×',
          onclick: () => {
            state.sequence.splice(i, 1);
            renderSequence();
          },
        })
      )
    );
  });

  $('#btn-keys-send').textContent =
    state.sequence.length === 1 ? 'Send combo' : `Send ${state.sequence.length} combos`;
}

async function onCustomChord() {
  const input = el('input', {
    class: 'input',
    type: 'text',
    placeholder: 'Ctrl+Alt+F4',
    autocapitalize: 'off',
    autocomplete: 'off',
    spellcheck: 'false',
  });
  const error = el('p', { class: 'inline-error', hidden: true });
  const wrap = el('div', {}, input, error);

  setTimeout(() => input.focus(), 120);
  const ok = await sheet({
    title: 'Custom combo',
    body: 'Keys pressed together, e.g. Ctrl+Alt+F4. Use Win for the Windows/Command key.',
    extra: wrap,
    actions: [
      { label: 'Add', value: true, style: 'primary' },
      { label: 'Cancel', value: false },
    ],
  });
  if (!ok) return;

  try {
    const chord = parseChord(input.value);
    if (state.sequence.length >= cmd.LIMITS.keysMaxChords) {
      toast('err', 'Sequence full', `The limit is ${cmd.LIMITS.keysMaxChords} chords.`);
      return;
    }
    state.sequence.push(chord);
    renderSequence();
  } catch (err) {
    toast('err', 'Not a valid combo', err.message);
  }
}

async function onSendKeys() {
  if (!state.sequence.length) return;
  const seq = state.sequence.map((c) => [...c]);
  await guard(async () => {
    const r = await cmd.sendKeys(state.device, seq);
    if (r.ok) {
      toast('ok', 'Keys sent', seq.map(prettyChord).join(' › '));
      state.sequence = [];
      renderSequence();
    } else {
      toast('err', 'Rejected', errorMessage(r.err, r.res));
    }
  }, 'Send failed');
}

/* ── power ────────────────────────────────────────────────────────── */

async function onPowerTap() {
  await guard(async () => {
    const r = await cmd.powerTap(state.device, cmd.DEFAULTS.power_tap);
    if (r.ok) toast('ok', 'Power tapped', '200 ms pulse on the power switch.');
    else toast('err', 'Rejected', errorMessage(r.err, r.res));
  }, 'Power tap failed');
}

/**
 * Step 2 of the destructive flow: the 1.5 s physical hold has completed, now
 * the user must confirm in a sheet before anything is signed or sent.
 */
async function onPowerHoldArmed() {
  const seconds = cmd.DEFAULTS.power_hold / 1000;
  const confirmed = await confirmDestructive({
    title: 'Force power off?',
    body:
      `This holds the physical power button for ${seconds} seconds. The PC loses ` +
      'power immediately — unsaved work is lost and the filesystem is not ' +
      'cleanly unmounted. Use Tap first if the machine is responsive.',
    confirmLabel: `Hold power for ${seconds}s`,
  });
  if (!confirmed) return;

  await guard(async () => {
    const r = await cmd.powerHold(state.device, cmd.DEFAULTS.power_hold);
    if (r.ok) toast('ok', 'Power held', `${seconds} s pulse sent.`);
    else toast('err', 'Rejected', errorMessage(r.err, r.res));
  }, 'Force off failed');
}

/* ══════════════════════════════════════════════════════ pair view ══ */

function bindPair() {
  $('#btn-pair-save').addEventListener('click', onPairSave);
  $('#btn-copy-pubkey').addEventListener('click', () => copyPubkey());
  $('#btn-copy-pubkey-2').addEventListener('click', () => copyPubkey());
  $('#btn-qr-pubkey').addEventListener('click', togglePubkeyQR);
  $('#pair-input').addEventListener('input', () => {
    $('#pair-error').hidden = true;
  });
}

function renderPair() {
  $('#pubkey-value').textContent = state.key?.publicKeyB64url || 'unavailable';
}

async function copyPubkey() {
  const key = state.key?.publicKeyB64url;
  if (!key) return;
  const ok = await copyText(key);
  toast(ok ? 'ok' : 'err', ok ? 'Public key copied' : 'Could not copy', ok ? '' : 'Select and copy it manually.');
}

function togglePubkeyQR() {
  const holder = $('#qr-holder');
  if (!holder.hidden) {
    holder.hidden = true;
    holder.replaceChildren();
    return;
  }
  try {
    holder.innerHTML = toSVG(state.key.publicKeyB64url);
    holder.append(el('p', { class: 'footer-note', text: 'Scan to carry the key to another device.' }));
    holder.hidden = false;
  } catch (err) {
    toast('err', 'Could not render QR', err.message);
  }
}

async function onPairSave() {
  const errNode = $('#pair-error');
  errNode.hidden = true;
  let parsed;
  try {
    parsed = parsePairingBlob($('#pair-input').value);
  } catch (err) {
    errNode.textContent = err.message;
    errNode.hidden = false;
    return;
  }

  const name = $('#pair-name').value.trim() || parsed.name || 'My PC';
  // One key per phone; `kid` is the slot THIS device filed it under. v1's
  // /pair reply does not report the slot, so default to p1 (correctable in
  // Settings) unless the firmware volunteered one.
  const kid = parsed.kid || 'p1';

  const device = await store.saveDevice({ ...parsed, name, kid });
  await store.setActiveDevice(device.deviceId);
  state.device = device;

  $('#pair-input').value = '';
  $('#pair-name').value = '';

  // Offer the biometric gate right after pairing, when its value is obvious.
  await offerWebAuthn();

  poller?.stop();
  poller?.start();
  showView('home');
  toast('ok', 'Device paired', `${name} is ready.`);
}

async function offerWebAuthn() {
  if (await webauthn.isGateActive()) return;
  if (!(await webauthn.hasPlatformAuthenticator())) return;

  const yes = await sheet({
    title: 'Lock this with your face or fingerprint?',
    body:
      'A stolen phone could otherwise type your password into your PC or hold ' +
      'its power button. The check happens on this phone only — nothing is sent ' +
      'to the device or the relay.',
    actions: [
      { label: 'Turn on', value: true, style: 'primary' },
      { label: 'Not now', value: false },
    ],
  });
  if (!yes) return;

  const result = await webauthn.register();
  if (result === 'registered') toast('ok', 'Biometric gate on');
  else if (result === 'declined') toast('info', 'Left off', 'You can turn it on in Settings.');
  else toast('info', 'Not available', 'This device has no platform authenticator.');
}

/* ══════════════════════════════════════════════════ settings view ══ */

function bindSettings() {
  $('#toggle-webauthn').addEventListener('change', onToggleWebAuthn);
  $('#toggle-haptics').addEventListener('change', async (e) => {
    await store.setSetting('hapticsEnabled', e.target.checked);
    state.settings.hapticsEnabled = e.target.checked;
    setHapticsEnabled(e.target.checked);
  });
  $('#toggle-reveal').addEventListener('change', (e) => {
    $('#unlock-text').type = e.target.checked ? 'text' : 'password';
  });
  $('#btn-unlock-save').addEventListener('click', onSaveUnlock);
  $('#btn-unlock-clear').addEventListener('click', onClearUnlock);
  $('#key-kid').addEventListener('change', onChangeKid);
  $('#btn-selftest').addEventListener('click', onSelfTest);
  $('#btn-add-device').addEventListener('click', () => showView('pair'));
  $('#btn-wipe').addEventListener('click', onWipe);
}

async function renderSettings() {
  state.settings = await store.getSettings();

  const supported = await webauthn.hasPlatformAuthenticator();
  const gate = $('#toggle-webauthn');
  gate.checked = await webauthn.isGateActive();
  gate.disabled = !supported;
  $('#webauthn-sub').textContent = supported
    ? 'Face / fingerprint before any command is signed. Checked on this phone only — never transmitted.'
    : 'Unavailable: this device has no platform authenticator (or the page is not on https).';

  $('#toggle-haptics').checked = state.settings.hapticsEnabled !== false;

  // unlock text
  $('#unlock-text').value = state.device?.unlockText || '';
  $('#toggle-enter').checked = state.device?.unlockEnter !== false;
  $('#toggle-reveal').checked = false;
  $('#unlock-text').type = 'password';

  // key
  const key = state.key || (await crypto_.getKeyInfo());
  if (key) {
    $('#key-kid').value = state.device?.kid || 'p1';
    $('#key-kid').disabled = !state.device;
    $('#key-mode').textContent = key.mode === 'webcrypto' ? 'WebCrypto (non-extractable)' : 'noble (software)';
    $('#key-mode-note').textContent =
      key.mode === 'webcrypto'
        ? 'The private key is held by the platform keystore and cannot be read or exported by this app.'
        : 'This browser has no Ed25519 in WebCrypto, so the private key is stored as raw bytes in IndexedDB — weaker: anything with script access to this origin could read it.';
    $('#settings-pubkey').textContent = key.publicKeyB64url;
    $('#key-ctr').textContent = state.device
      ? String(await store.peekCounter(store.counterKey(state.device.deviceId, state.device.kid || 'p1')))
      : '—';
  }

  renderDeviceList();
}

async function renderDeviceList() {
  const host = $('#device-list');
  const devices = await store.listDevices();
  host.replaceChildren();

  if (!devices.length) {
    host.append(el('p', { class: 'card-sub', text: 'No devices paired yet.' }));
    return;
  }

  for (const d of devices) {
    const active = d.deviceId === state.device?.deviceId;
    host.append(
      el(
        'div',
        {
          class: 'device-row',
          'data-active': String(active),
          onclick: async () => {
            await store.setActiveDevice(d.deviceId);
            state.device = await store.getActiveDevice();
            poller?.refresh();
            renderDeviceList();
            toast('info', `Switched to ${d.name}`);
          },
        },
        el(
          'div',
          { class: 'device-row-body' },
          el('div', { class: 'device-row-name', text: d.name || d.deviceId }),
          el('div', { class: 'device-row-id mono', text: d.deviceId })
        ),
        active ? el('span', { class: 'pill', text: 'Active' }) : null,
        el('button', {
          class: 'icon-btn',
          type: 'button',
          'aria-label': `Remove ${d.name}`,
          text: '×',
          onclick: async (e) => {
            e.stopPropagation();
            const ok = await confirmDestructive({
              title: `Remove ${d.name}?`,
              body: 'This forgets the device token and relay URL on this phone. The device keeps your public key until you clear the slot in setup mode.',
              confirmLabel: 'Remove',
            });
            if (!ok) return;
            await store.deleteDevice(d.deviceId);
            state.device = await store.getActiveDevice();
            renderDeviceList();
            if (!state.device) showView('pair');
          },
        })
      )
    );
  }
}

async function onToggleWebAuthn(e) {
  if (e.target.checked) {
    const result = await webauthn.enableGate();
    if (result !== 'registered') {
      e.target.checked = false;
      toast(
        result === 'declined' ? 'info' : 'err',
        result === 'declined' ? 'Cancelled' : 'Not available',
        result === 'declined' ? '' : 'No platform authenticator on this device.'
      );
      return;
    }
    toast('ok', 'Biometric gate on');
  } else {
    const ok = await confirmDestructive({
      title: 'Turn off the biometric gate?',
      body: 'Anyone holding this unlocked phone will be able to type your unlock text into your PC and hold its power button.',
      confirmLabel: 'Turn off',
    });
    if (!ok) {
      e.target.checked = true;
      return;
    }
    await webauthn.disableGate();
    toast('info', 'Biometric gate off');
  }
}

/**
 * Sign a throwaway command with this phone's key and verify it against the
 * public key we hand to the device. Nothing is sent anywhere. This is the
 * quickest way to confirm, on a real phone, that the chosen crypto backend
 * actually works end-to-end before blaming the device or the relay.
 */
async function onSelfTest() {
  const btn = $('#btn-selftest');
  btn.disabled = true;
  try {
    const key = await crypto_.ensureKey();
    const command = {
      dev: state.device?.deviceId || '0000000000000000',
      id: uuid4(),
      ts: nowSec(),
      ctr: 0,
      act: 'ping',
      args: {},
    };
    const sig = await crypto_.signCommand(command);
    const ok = await crypto_.verifyCommand(command, sig, key.publicKeyRaw);
    if (ok) {
      toast('ok', 'Signing works', `${key.mode === 'webcrypto' ? 'WebCrypto' : 'noble'} produced a valid signature.`);
    } else {
      toast('err', 'Self-test failed', 'The signature did not verify against this key.');
    }
  } catch (err) {
    toast('err', 'Self-test failed', err?.message || 'Unexpected error.');
  } finally {
    btn.disabled = false;
  }
}

/**
 * Correct the operator slot. The counter is scoped to (device, slot), so
 * switching slots starts a fresh counter — which is right: the device tracks a
 * separate high-water mark for each slot.
 */
async function onChangeKid(e) {
  if (!state.device) return;
  const kid = e.target.value;
  state.device = await store.saveDevice({ deviceId: state.device.deviceId, kid });
  $('#key-ctr').textContent = String(
    await store.peekCounter(store.counterKey(state.device.deviceId, kid))
  );
  toast('info', `Signing as slot ${kid}`);
}

async function onSaveUnlock() {
  if (!state.device) {
    toast('err', 'Pair a device first');
    return;
  }
  const text = $('#unlock-text').value;
  if (text.length > cmd.LIMITS.typeMaxChars) {
    toast('err', 'Too long', `The type action accepts at most ${cmd.LIMITS.typeMaxChars} characters.`);
    return;
  }
  // Storing the secret is itself gated — it is the asset, not just its use.
  try {
    await webauthn.requirePresence();
  } catch {
    toast('err', 'Cancelled', 'Identity check is required to change the unlock text.');
    return;
  }

  state.device = await store.saveDevice({
    deviceId: state.device.deviceId,
    unlockText: text,
    unlockEnter: $('#toggle-enter').checked,
  });
  renderUnlockCard();
  toast(
    text ? 'ok' : 'info',
    text ? 'Unlock text saved' : 'Unlock text cleared',
    text ? 'Stored on this phone. The relay sees it when you send it.' : ''
  );
}

async function onClearUnlock() {
  if (!state.device?.unlockText) {
    $('#unlock-text').value = '';
    return;
  }
  const ok = await confirmDestructive({
    title: 'Remove the unlock text?',
    body: 'It will be erased from this phone.',
    confirmLabel: 'Remove',
  });
  if (!ok) return;
  state.device = await store.saveDevice({ deviceId: state.device.deviceId, unlockText: '' });
  $('#unlock-text').value = '';
  renderUnlockCard();
  toast('ok', 'Unlock text removed');
}

async function onWipe() {
  const ok = await confirmDestructive({
    title: 'Erase everything?',
    body: 'The signing key, paired devices, counters and stored unlock text are deleted from this phone. You will need to pair again — and to remove the old key slot on the device in setup mode.',
    confirmLabel: 'Erase everything',
  });
  if (!ok) return;
  await store.wipeAll();
  poller?.stop();
  state.device = null;
  state.key = await crypto_.ensureKey();
  state.sequence = [];
  toast('ok', 'Erased');
  showView('pair');
}

/* ═════════════════════════════════════════════════════════ helpers ══ */

/**
 * Single funnel for every command-sending handler: serialises actions, keeps
 * the UI honest about busy state, and turns thrown errors into human messages.
 */
async function guard(fn, failTitle, onFail) {
  if (state.busy) return;
  if (!state.device) {
    toast('err', 'No device paired');
    return;
  }
  state.busy = true;
  setBusy(true);
  try {
    haptic();
    await fn();
  } catch (err) {
    onFail?.();
    handleError(err, failTitle);
  } finally {
    state.busy = false;
    setBusy(false);
  }
}

function setBusy(busy) {
  $('#btn-wake').classList.toggle('busy', busy);
  for (const id of ['#btn-wake', '#btn-unlock', '#btn-power-tap', '#btn-keys-send', '#btn-power-hold']) {
    const node = $(id);
    if (!node) continue;
    if (id === '#btn-unlock' && !state.device?.unlockText) continue;
    node.disabled = busy;
  }
}

function handleError(err, title) {
  console.error(title, err);
  if (err?.message === 'user-presence-declined') {
    toast('info', 'Cancelled', 'Identity check was not completed.');
    return;
  }
  if (err instanceof RelayError) {
    toast('err', title, errorMessage(err.code));
    return;
  }
  toast('err', title, err?.message || 'Unexpected error.');
}

/* ═══════════════════════════════════════════════════════════ start ══ */

boot();
