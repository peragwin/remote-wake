// Throwaway passkey spike (docs/design/passkey-setup.md, Phase 0). Answers:
// does a passkey made in the browser work from the Home Screen app, does a
// discoverable get() return userHandle there, how often does UV prompt, and
// is ES256 what we get. Nothing leaves the page.

const $ = (id) => document.getElementById(id);
const logEl = $('log');

// Community AAGUID list (passkey-authenticator-aaguids).
const AAGUIDS = {
  'fbfc3007-154e-4ecc-8c0b-6e020557d7bd': 'iCloud Keychain',
  'ea9b8d66-4d01-1d21-3ce4-b6b48cb575d4': 'Google Password Manager',
  '00000000-0000-0000-0000-000000000000': 'zeroed (provider hidden)',
};

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
const b64u = (buf) => btoa(String.fromCharCode(...new Uint8Array(buf)))
  .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const fromB64u = (s) => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/')), (c) => c.charCodeAt(0));
const rand = (n) => crypto.getRandomValues(new Uint8Array(n));

function log(line) {
  const t = new Date().toLocaleTimeString();
  logEl.textContent += `[${t}] ${line}\n`;
}

function store(key, value) {
  try { localStorage.setItem(key, value); } catch { /* private mode */ }
}
function load(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}

function flags(ad) {
  const f = new Uint8Array(ad)[32];
  const on = [];
  if (f & 0x01) on.push('UP');
  if (f & 0x04) on.push('UV');
  if (f & 0x08) on.push('BE');
  if (f & 0x10) on.push('BS');
  if (f & 0x40) on.push('AT');
  return on.join(' ') || 'none';
}

function aaguid(ad) {
  const b = new Uint8Array(ad);
  if (b.length < 53) return 'n/a';
  const h = hex(b.slice(37, 53));
  const id = `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
  return `${id} (${AAGUIDS[id] || 'unknown provider'})`;
}

async function context() {
  const standalone = matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;
  let uvpa = 'n/a';
  try { uvpa = await PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable(); } catch { /* old */ }
  $('ctx').textContent = [
    `mode: ${standalone ? 'HOME SCREEN APP (standalone)' : 'browser tab'}`,
    `origin: ${location.origin}`,
    `platform authenticator: ${uvpa}`,
    `passkey made in THIS context: ${load('spike.cred') ? 'yes' : 'no'}`,
    `ua: ${navigator.userAgent}`,
  ].join('\n');
  return standalone ? 'app' : 'tab';
}

async function create() {
  const handle = rand(41);
  handle[0] = 1;
  const t0 = performance.now();
  const cred = await navigator.credentials.create({
    publicKey: {
      rp: { id: location.hostname, name: 'remote-wake test' },
      user: { id: handle, name: 'Voidstats (test)', displayName: 'Voidstats (test)' },
      challenge: rand(32),
      pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
      authenticatorSelection: { residentKey: 'required', userVerification: 'required' },
      attestation: 'none',
      timeout: 120000,
    },
  });
  const ms = Math.round(performance.now() - t0);
  const r = cred.response;
  const alg = r.getPublicKeyAlgorithm ? r.getPublicKeyAlgorithm() : 'n/a';
  const ad = r.getAuthenticatorData ? r.getAuthenticatorData() : null;
  store('spike.cred', cred.id);
  store('spike.handle', hex(handle));
  log(`CREATE ok in ${ms} ms in ${await context()}`);
  log(`  alg ${alg} (${alg === -7 ? 'ES256 as wanted' : 'NOT ES256'}), attachment ${cred.authenticatorAttachment}`);
  if (ad) log(`  flags ${flags(ad)}; provider ${aaguid(ad)}`);
  log(`  handle ${hex(handle).slice(0, 18)}…`);
}

async function get(allow) {
  const t0 = performance.now();
  const a = await navigator.credentials.get({
    publicKey: {
      challenge: rand(32),
      rpId: location.hostname,
      userVerification: 'required',
      allowCredentials: allow ? [{ type: 'public-key', id: fromB64u(allow) }] : [],
      timeout: 120000,
    },
  });
  return { a, ms: Math.round(performance.now() - t0) };
}

async function find() {
  const where = await context();
  const { a, ms } = await get(null);
  const uh = a.response.userHandle ? hex(a.response.userHandle) : null;
  store('spike.found', a.id);
  log(`FIND ok in ${ms} ms in ${where}`);
  log(`  userHandle ${uh ? `${uh.slice(0, 18)}… (${uh.length / 2} bytes)` : 'MISSING'}`);
  const made = load('spike.handle');
  if (made) log(`  matches the one created here: ${made === uh}`);
  log(`  flags ${flags(a.response.authenticatorData)}`);
}

async function signThree() {
  const where = await context();
  const id = load('spike.found') || load('spike.cred');
  if (!id) { log('SIGN: run Find first'); return; }
  for (let i = 1; i <= 3; i++) {
    const { a, ms } = await get(id);
    log(`SIGN ${i}/3 ok in ${ms} ms in ${where}; flags ${flags(a.response.authenticatorData)}`);
  }
  log('  → how many Face ID prompts did you see?');
}

function guard(fn) {
  return async () => {
    try { await fn(); } catch (e) { log(`ERROR ${e.name}: ${e.message}`); }
  };
}

$('b-create').addEventListener('click', guard(create));
$('b-find').addEventListener('click', guard(find));
$('b-sign').addEventListener('click', guard(signThree));
$('b-clear').addEventListener('click', () => { logEl.textContent = ''; });
context();
