/**
 * IndexedDB persistence.
 *
 * Stores:
 *   devices   keyPath 'deviceId'  → { deviceId, deviceToken, relayUrl, name, kid,
 *                                     addedAt, unlockText?, unlockEnter?,
 *                                     acts? (last status `acts`; see caps.js) }
 *   keys      keyPath 'kid'       → { kid, mode:'webcrypto'|'noble',
 *                                     privateKey|seed, publicKeyRaw, createdAt }
 *                                     (one record, id 'phone' — see crypto.js)
 *   counters  keyPath 'kid'       → { kid, ctr }   kid = "<deviceId>:<slot>"
 *   settings  keyPath 'k'         → { k, v }
 *
 * The counter is the replay defence's phone-side half (protocol §ctr): it is
 * incremented AND persisted BEFORE a command is signed, so a crash or a
 * double-tap can only skip counter values, never reuse one.
 */

const DB_NAME = 'remote-wake';
const DB_VERSION = 1;
const STORES = ['devices', 'keys', 'counters', 'settings'];

let dbPromise = null;

function openDB() {
  if (dbPromise) return dbPromise;
  dbPromise = new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains('devices')) db.createObjectStore('devices', { keyPath: 'deviceId' });
      if (!db.objectStoreNames.contains('keys')) db.createObjectStore('keys', { keyPath: 'kid' });
      if (!db.objectStoreNames.contains('counters')) db.createObjectStore('counters', { keyPath: 'kid' });
      if (!db.objectStoreNames.contains('settings')) db.createObjectStore('settings', { keyPath: 'k' });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
    req.onblocked = () => reject(new Error('IndexedDB upgrade blocked — close other tabs'));
  });
  return dbPromise;
}

function tx(store, mode, fn) {
  return openDB().then(
    (db) =>
      new Promise((resolve, reject) => {
        const t = db.transaction(store, mode);
        const req = fn(t.objectStore(store));
        t.oncomplete = () => resolve(req ? req.result : undefined);
        t.onerror = () => reject(t.error);
        t.onabort = () => reject(t.error || new Error('transaction aborted'));
      })
  );
}

const get = (store, key) => tx(store, 'readonly', (s) => s.get(key));
const getAll = (store) => tx(store, 'readonly', (s) => s.getAll());
const put = (store, value) => tx(store, 'readwrite', (s) => s.put(value));
const del = (store, key) => tx(store, 'readwrite', (s) => s.delete(key));

/* ---------------------------------------------------------------- devices */

export const listDevices = () => getAll('devices');
export const getDevice = (deviceId) => get('devices', deviceId);
export const deleteDevice = (deviceId) => del('devices', deviceId);

export async function saveDevice(device) {
  const existing = await getDevice(device.deviceId);
  await put('devices', { addedAt: Date.now(), kid: 'p1', ...existing, ...device });
  return getDevice(device.deviceId);
}

/** The single-device primary UX: the active device, or the only/first one. */
export async function getActiveDevice() {
  const devices = await listDevices();
  if (!devices.length) return null;
  const id = await getSetting('activeDeviceId');
  return devices.find((d) => d.deviceId === id) || devices[0];
}

export const setActiveDevice = (deviceId) => setSetting('activeDeviceId', deviceId);

/* ------------------------------------------------------------------ keys */

export const getKeyRecord = (kid) => get('keys', kid);
export const listKeyRecords = () => getAll('keys');
export const putKeyRecord = (rec) => put('keys', rec);
export const deleteKeyRecord = (kid) => del('keys', kid);

/* -------------------------------------------------------------- counters */

/**
 * The counter is scoped to (device, slot), because the device's high-water
 * mark is per key slot in ITS OWN NVS — two paired devices track independent
 * counters even though this phone signs both with one key.
 */
export const counterKey = (deviceId, kid) => `${deviceId}:${kid}`;

/**
 * Increment-and-persist, then return the value to sign with. Awaiting this
 * before signing is what makes the write-then-use ordering real.
 */
export async function nextCounter(kid) {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const t = db.transaction('counters', 'readwrite');
    const store = t.objectStore('counters');
    const read = store.get(kid);
    let next;
    read.onsuccess = () => {
      const cur = read.result?.ctr ?? 0;
      next = cur + 1;
      store.put({ kid, ctr: next });
    };
    t.oncomplete = () => resolve(next);
    t.onerror = () => reject(t.error);
    t.onabort = () => reject(t.error || new Error('counter transaction aborted'));
  });
}

export async function peekCounter(kid) {
  return (await get('counters', kid))?.ctr ?? 0;
}

/** Largest forward jump a `replay` reply may cause, and the absolute ceiling. */
export const COUNTER_MAX_JUMP = 1_000_000;
export const COUNTER_CEILING = 2 ** 52;

/**
 * Whether a device-reported high-water mark may move the local counter from
 * @p cur. The value arrives in an unsigned relay reply, so a hostile relay
 * could otherwise push the counter to (or past) Number.MAX_SAFE_INTEGER and
 * leave this phone unable to sign another accepted command. Only a safe
 * integer, below 2^52, ahead of @p cur by at most COUNTER_MAX_JUMP counts.
 */
export function acceptFastForward(cur, value) {
  return (
    Number.isSafeInteger(value) &&
    value < COUNTER_CEILING &&
    value > cur &&
    value - cur <= COUNTER_MAX_JUMP
  );
}

/**
 * Jump the counter forward — used when the device reports `replay`, meaning its
 * high-water mark is ahead of ours (e.g. after restoring the app from a backup).
 * Anything acceptFastForward rejects is ignored. @returns {Promise<boolean>}
 */
export async function bumpCounterTo(kid, value) {
  const cur = await peekCounter(kid);
  if (!acceptFastForward(cur, value)) return false;
  await put('counters', { kid, ctr: value });
  return true;
}

/* -------------------------------------------------------------- settings */

const DEFAULT_SETTINGS = {
  activeDeviceId: null,
  webauthnEnabled: true,
  webauthnCredentialId: null,
  pollPresence: true,
  hapticsEnabled: true,
};

export async function getSetting(key) {
  const row = await get('settings', key);
  return row ? row.v : DEFAULT_SETTINGS[key];
}

export const setSetting = (key, value) => put('settings', { k: key, v: value });

export async function getSettings() {
  const rows = await getAll('settings');
  const out = { ...DEFAULT_SETTINGS };
  for (const r of rows) out[r.k] = r.v;
  return out;
}

/** Factory reset: wipes keys, devices, counters and settings. */
export async function wipeAll() {
  const db = await openDB();
  return new Promise((resolve, reject) => {
    const t = db.transaction(STORES, 'readwrite');
    for (const s of STORES) t.objectStore(s).clear();
    t.oncomplete = resolve;
    t.onerror = () => reject(t.error);
  });
}
