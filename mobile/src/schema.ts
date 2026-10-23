// schema.ts — what a signed index or pointer may contain (audit 2026-10-23: BP-4, BP-6, BP-7, BP-11,
// BP-12, BP-18, BP-22). A valid signature only says the repo's key signed it; the repo can still sign
// garbage, so every field is checked, every list is capped, and anything that becomes a file name, a
// URL or a native argument must match a strict pattern. An index that fails is refused and never stored.

export const LIMITS = { indexBytes: 1 << 20, apps: 500, versions: 5, nodes: 8, addrs: 4, pointers: 4, iconBytes: 512 << 10 };

const PKG = /^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$/;
const CID = /^z[1-9A-HJ-NP-Za-km-z]{40,90}$/;            // base58btc CIDv1, as Logos Storage prints them
const HEX64 = /^[0-9a-f]{64}$/;
const KEY = /^[A-Za-z0-9_-]{43}$/;                        // ed25519 public key, base64url
const PEER = /^[1-9A-HJ-NP-Za-km-z]{40,80}$/;             // libp2p peer id (base58)

export interface Peer { peerId: string; addrs: string[]; spr?: string }
export interface Version { versionCode: number; versionName: string; size: number; sha256: string; signer: string; cid: string; minSdk: number; abis: string[]; added?: string; blockSize?: number }
export interface App { id: string; label: string; summary?: string; description?: string; license?: string; source?: string; icon: string | null; versions: Version[] }
export interface Index { v: number; name: string; key: string; seq: number; created: string; nodes: Peer[]; pointers: string[]; apps: App[] }
export interface Pointer { key: string; seq: number; index: string; created: string; nodes: Peer[] }

class Bad extends Error {}
const fail = (what: string): never => { throw new Bad(`invalid repository data: ${what}`); };
const str = (v: unknown, max: number, what: string, optional = false): string | undefined => {
  if (v === undefined || v === null) return optional ? undefined : fail(`${what} missing`);
  if (typeof v !== "string" || v.length > max) return fail(`${what} is not a string of at most ${max} characters`);
  return v;
};
const int = (v: unknown, what: string, min = 0, max = Number.MAX_SAFE_INTEGER): number =>
  Number.isInteger(v) && (v as number) >= min && (v as number) <= max ? (v as number) : fail(`${what} is not an integer in range`);
const arr = <T>(v: unknown, max: number, what: string, item: (x: unknown, i: number) => T): T[] =>
  Array.isArray(v) && v.length <= max ? v.map(item) : fail(`${what} is not a list of at most ${max}`);
const match = (v: unknown, re: RegExp, what: string): string => (typeof v === "string" && re.test(v) ? v : fail(`${what} is malformed`));

/** Only plain web links may be opened from a repo's data (BP-18). */
export const httpUrl = (v: unknown): string | undefined => (typeof v === "string" && v.length <= 500 && /^https?:\/\/[^\s]+$/i.test(v) ? v : undefined);

/** HTTPS pointer URL to a public host only (BP-11): no plain http, no loopback, private or link-local. */
export function safePointerUrl(u: unknown): string | undefined {
  if (typeof u !== "string" || u.length > 500) return undefined;
  let url: URL;
  try { url = new URL(u); } catch { return undefined; }
  if (url.protocol !== "https:" || url.username || url.password) return undefined;
  const h = url.hostname.toLowerCase().replace(/^\[|\]$/g, "");
  if (h === "localhost" || h.endsWith(".localhost") || h.endsWith(".local") || h.endsWith(".internal")) return undefined;
  if (/^(127\.|10\.|0\.|169\.254\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.)/.test(h)) return undefined;
  if (h === "::1" || h === "::" || /^(fc|fd|fe8|fe9|fea|feb)/.test(h)) return undefined;
  return url.toString();
}

export function peer(x: unknown, i = 0): Peer {
  const p = x as any;
  if (!p || typeof p !== "object") fail(`node ${i}`);
  return {
    peerId: match(p.peerId, PEER, `node ${i} peer id`),
    addrs: arr(p.addrs, LIMITS.addrs, `node ${i} addresses`, (a, j) => match(a, /^\/[A-Za-z0-9.:/_-]{3,200}$/, `node ${i} address ${j}`)),
    spr: p.spr === undefined ? undefined : match(p.spr, /^spr:[A-Za-z0-9_-]{10,2000}$/, `node ${i} SPR`),
  };
}

function version(x: unknown, where: string): Version {
  const v = x as any;
  if (!v || typeof v !== "object") fail(where);
  return {
    versionCode: int(v.versionCode, `${where} versionCode`, 1, 2 ** 31 - 1),
    versionName: str(v.versionName, 100, `${where} versionName`)!,
    size: int(v.size, `${where} size`, 1, 1 << 30),
    sha256: match(v.sha256, HEX64, `${where} sha256`),
    signer: match(v.signer, HEX64, `${where} signer (every version must pin its signing certificate)`),
    cid: match(v.cid, CID, `${where} cid`),
    minSdk: int(v.minSdk ?? 21, `${where} minSdk`, 1, 100),
    abis: arr(v.abis ?? [], 8, `${where} abis`, (a) => match(a, /^[a-z0-9_-]{2,20}$/, `${where} abi`)),
    added: str(v.added, 40, `${where} added`, true),
    blockSize: v.blockSize === undefined ? undefined : int(v.blockSize, `${where} blockSize`, 4096, 1 << 20),
  };
}

function app(x: unknown, i: number): App {
  const a = x as any;
  if (!a || typeof a !== "object") fail(`app ${i}`);
  const id = match(a.id, PKG, `app ${i} package name`);
  if (id.length > 200) fail(`app ${i} package name too long`);
  const label = str(a.label, 100, `${id} label`)!;
  if (!label.trim()) fail(`${id} has an empty label`);
  const versions = arr(a.versions, LIMITS.versions, `${id} versions`, (v, j) => version(v, `${id} version ${j}`));
  if (!versions.length) fail(`${id} has no versions`);
  return {
    id, label, versions,
    summary: str(a.summary, 300, `${id} summary`, true),
    description: str(a.description, 8000, `${id} description`, true),
    license: str(a.license, 100, `${id} license`, true),
    source: httpUrl(a.source),
    icon: a.icon === null || a.icon === undefined ? null : match(a.icon, CID, `${id} icon`),
  };
}

/** Check a verified index body; returns a cleaned copy or throws. `key` is the repo key it was opened with. */
export function validateIndex(b: any, key: string): Index {
  if (!b || typeof b !== "object" || Array.isArray(b)) fail("index is not an object");
  if (b.key !== key) fail("index names a different repo");
  if (b.index !== undefined) fail("a pointer, not an index");
  const apps = arr(b.apps, LIMITS.apps, "apps", app);
  if (new Set(apps.map((a) => a.id)).size !== apps.length) fail("an app is listed twice");
  return {
    v: int(b.v, "index version", 1, 2), name: str(b.name, 100, "repository name")!, key,
    seq: int(b.seq ?? 0, "seq"), created: str(b.created, 40, "created")!,
    nodes: arr(b.nodes ?? [], LIMITS.nodes, "nodes", peer),
    pointers: (Array.isArray(b.pointers) ? b.pointers.slice(0, LIMITS.pointers) : []).map(safePointerUrl).filter((u: string | undefined): u is string => !!u),
    apps,
  };
}

/** Check a verified pointer body; returns a cleaned copy or throws. */
export function validatePointer(b: any, key: string): Pointer {
  if (!b || typeof b !== "object" || Array.isArray(b)) fail("pointer is not an object");
  if (b.key !== key) fail("pointer names a different repo");
  return {
    key, seq: int(b.seq, "pointer seq"), index: match(b.index, CID, "pointer index cid"),
    created: str(b.created, 40, "pointer created")!, nodes: arr(b.nodes ?? [], LIMITS.nodes, "pointer nodes", peer),
  };
}

export const isKey = (k: unknown): k is string => typeof k === "string" && KEY.test(k);
export const isCid = (c: unknown): c is string => typeof c === "string" && CID.test(c);
