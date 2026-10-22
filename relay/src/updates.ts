/** Public release discovery only. A hint has no firmware-signing authority. */
export const UPDATE_REFRESH_MS = 5 * 60_000;
export const UPDATE_RETRY_MS = 30_000;
export const MAX_CATALOG_BYTES = 64 * 1024;
export const MAX_RELEASES = 64;
const FETCH_TIMEOUT_MS = 5_000;
const ID = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;
const REPO = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/;
const fields = ["product", "board", "profile", "channel", "partition_layout"] as const;
const limits = [31, 31, 47, 15, 31] as const;

export interface Registration {
  v: 1;
  updater_api: 1;
  product: string;
  board: string;
  profile: string;
  channel: string;
  partition_layout: string;
  version: string;
  release_counter: number;
}
export interface Release {
  product: string;
  board: string;
  profile: string;
  channel: string;
  partition_layout: string;
  version: string;
  release_counter: number;
  manifest_url: string;
}
export interface UpdateSource {
  catalog: string;
  repos: ReadonlySet<string>;
}
export interface UpdateBindings {
  UPDATE_CATALOG_URL?: string;
  UPDATE_ALLOWED_REPOS?: string;
}
export interface UpdateAttachment {
  registration: Registration;
  checked_at: number;
  manifest: string | null;
}
function object(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}
function counter(value: unknown): value is number {
  return Number.isInteger(value) && typeof value === "number" && value >= 0 && value <= 0xffffffff;
}
function identity(value: Record<string, unknown>): boolean {
  return fields.every((f, i) => typeof value[f] === "string" && value[f].length <= limits[i]! && ID.test(value[f]));
}
export function registration(value: unknown): Registration | null {
  if (!object(value) || value.v !== 1 || value.updater_api !== 1 || !identity(value) ||
      typeof value.version !== "string" || value.version.length > 31 || !ID.test(value.version) ||
      !counter(value.release_counter)) return null;
  return { v: 1, updater_api: 1, product: value.product as string, board: value.board as string,
    profile: value.profile as string, channel: value.channel as string,
    partition_layout: value.partition_layout as string, version: value.version,
    release_counter: value.release_counter };
}
/** Only operator-selected public GitHub repositories. Never fetch a device URL. */
function githubUrl(value: unknown, repos: ReadonlySet<string>, manifest: boolean): string | null {
  if (typeof value !== "string" || value.length > 511 || !/^[\x21-\x7e]+$/.test(value)) return null;
  try {
    const url = new URL(value);
    if (url.protocol !== "https:" || url.username || url.password || url.port || url.search || url.hash) return null;
    const parts = url.pathname.split("/");
    if (parts.some(p => p === "." || p === ".." || p.includes("%") || p.includes("\\"))) return null;
    const repo = `${parts[1]}/${parts[2]}`.toLowerCase();
    if (!repos.has(repo)) return null;
    if (url.hostname === "github.com") {
      if (parts[3] !== "releases" || parts[4] !== "download" || !parts[5] || !parts[6]) return null;
    } else if (url.hostname === "raw.githubusercontent.com") {
      if (manifest || !parts[3] || !parts[4]) return null;
    } else return null;
    return url.href;
  } catch { return null; }
}
export function updateSource(env: UpdateBindings): UpdateSource | null {
  if (!env.UPDATE_CATALOG_URL || !env.UPDATE_ALLOWED_REPOS || env.UPDATE_ALLOWED_REPOS.length > 2048) return null;
  const entries = env.UPDATE_ALLOWED_REPOS.split(",").map(x => x.trim().toLowerCase());
  if (entries.length > 32 || entries.some(x => !REPO.test(x))) return null;
  const repos = new Set(entries);
  const catalog = githubUrl(env.UPDATE_CATALOG_URL, repos, false);
  return catalog ? { catalog, repos } : null;
}
export function parseCatalog(value: unknown, source: UpdateSource): Release[] | null {
  if (!object(value) || value.v !== 1 || !Array.isArray(value.releases) || value.releases.length > MAX_RELEASES) return null;
  const releases: Release[] = [];
  for (const item of value.releases) {
    if (!object(item) || !identity(item) || !counter(item.release_counter) ||
        typeof item.version !== "string" || item.version.length > 31 || !ID.test(item.version)) return null;
    const manifest_url = githubUrl(item.manifest_url, source.repos, true);
    if (!manifest_url) return null;
    releases.push({ product: item.product as string, board: item.board as string, profile: item.profile as string,
      channel: item.channel as string, partition_layout: item.partition_layout as string,
      release_counter: item.release_counter, version: item.version, manifest_url });
  }
  return releases;
}
export function matchingRelease(releases: readonly Release[], device: Registration): Release | null {
  let best: Release | null = null;
  for (const release of releases) {
    if (fields.every(f => release[f] === device[f]) && release.release_counter > device.release_counter &&
        (!best || release.release_counter > best.release_counter)) best = release;
  }
  return best;
}
/** Bounded streaming read; reject overflow immediately rather than drain forever. */
async function readCatalog(response: Response): Promise<unknown> {
  if (!response.ok || !response.body) return null;
  const length = response.headers.get("content-length");
  if (length !== null && (!/^\d+$/.test(length) || Number(length) > MAX_CATALOG_BYTES)) {
    await response.body.cancel(); return null;
  }
  const reader = response.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > MAX_CATALOG_BYTES) { await reader.cancel(); return null; }
      chunks.push(value);
    }
    const data = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) { data.set(chunk, offset); offset += chunk.byteLength; }
    return JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(data)) as unknown;
  } catch { return null; }
  finally { reader.releaseLock(); }
}
/** Shared public metadata cache, never a token-bearing request or binary image. */
export async function fetchCatalog(source: UpdateSource): Promise<Release[] | null> {
  const cache = caches.default;
  const key = new Request(source.catalog);
  const hit = await cache.match(key);
  if (hit) {
    const parsed = parseCatalog(await readCatalog(hit), source);
    if (parsed) return parsed;
  }
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), FETCH_TIMEOUT_MS);
  try {
    const response = await fetch(source.catalog, {
      redirect: "manual", signal: controller.signal,
      headers: { accept: "application/json", "user-agent": "remote-wake-release-relay" },
    });
    // Raw GitHub catalogs avoid redirects. Never follow one to a new host.
    if (response.status >= 300 && response.status < 400) { await response.body?.cancel(); return null; }
    const parsed = parseCatalog(await readCatalog(response), source);
    if (!parsed) return null;
    await cache.put(key, new Response(JSON.stringify({ v: 1, releases: parsed }), {
      headers: { "content-type": "application/json", "cache-control": "public, max-age=120" },
    }));
    return parsed;
  } catch { return null; }
  finally { clearTimeout(timeout); }
}
