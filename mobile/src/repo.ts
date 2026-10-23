// repo.ts — Backpack repositories on the phone: parse a repo link, follow the repo's signed pointer,
// fetch + verify the signed index from Logos Storage, remember repos, verify + install APKs.
// Security model: docs/adr/0002-security-model.md; updates: docs/adr/0003-updates-signed-pointer.md.
//
// Link: backpack://repo?i=<indexCID>&k=<ed25519 key, b64url>&n=<name>&p=<peerId>&a=<addr>…&s=<spr>&u=<pointer url>…
// Envelope: {"backpack":1,["kind":"pointer",]"body":"<json>","sig":"<b64url ed25519 over body>"}
import AsyncStorage from "@react-native-async-storage/async-storage";
import { openEnvelope } from "./envelope";
export { openEnvelope };
import { NativeModules } from "react-native";
import * as storage from "./storage";
import * as delivery from "./delivery";
import * as settings from "./settings";
import { LIMITS, isCid, isKey, peer as checkPeer, safePointerUrl, validateIndex, validatePointer } from "./schema";
import type { App, Index, Peer, Pointer, Version } from "./schema";
export type { App, Index, Pointer, Version };

const Installer: any = (NativeModules as any).BackpackInstaller;

export interface Repo {
  key: string; name: string; indexCid: string; peers: storage.Peer[];
  seq?: number;            // highest seq accepted: never go below it (rollback protection)
  pointers?: string[];     // where to look for newer indexes (link u= + index.pointers)
  index?: Index; loadedCid?: string; fetchedAt?: string; checkedAt?: string; error?: string;
  sources?: Source[];      // what each pointer source said at the last check
  via?: string;            // the source of the pointer the current index came from
}
export interface Source { name: string; seq?: number; error?: string }

const KEY = "backpack.repos.v1";

const uniq = <T,>(xs: T[]) => [...new Set(xs)];

/** A link is UNTRUSTED input (any app can open one): every field is checked and capped (audit BP-9/11/12). */
export function parseLink(link: string): Repo {
  const t = link.trim();
  if (t.length > 8000 || !/^backpack:\/\/repo\?/i.test(t)) throw new Error("not a backpack:// repo link");
  const p = new URLSearchParams(t.slice(t.indexOf("?") + 1));
  const i = p.get("i"), k = p.get("k"), peerId = p.get("p");
  if (!i || !k || !peerId) throw new Error("the link is missing the index, key or node");
  if (!isKey(k)) throw new Error("the link's repository key is malformed");
  if (!isCid(i)) throw new Error("the link's index CID is malformed");
  let node: Peer;
  try { node = checkPeer({ peerId, addrs: p.getAll("a").slice(0, LIMITS.addrs), spr: p.get("s") ?? undefined }); }
  catch (e: any) { throw new Error(`the link's node is malformed (${e?.message ?? e})`); }
  const name = (p.get("n") ?? "Backpack").slice(0, 100);
  const pointers = p.getAll("u").slice(0, LIMITS.pointers).map(safePointerUrl).filter((u): u is string => !!u);
  return { key: k, name, indexCid: i, peers: [node], pointers, seq: 0 };
}

/** Stored repos are re-checked on load: an index that no longer validates is dropped (the repo stays,
 *  with an error, so it can be removed) instead of crashing every launch (audit BP-4). */
export async function loadRepos(): Promise<Repo[]> {
  let raw: any[];
  try { raw = JSON.parse((await AsyncStorage.getItem(KEY)) ?? "[]"); } catch { return []; }
  if (!Array.isArray(raw)) return [];
  const out: Repo[] = [];
  for (const r of raw) {
    if (!r || !isKey(r.key)) continue;
    const repo: Repo = { ...r, name: typeof r.name === "string" ? r.name.slice(0, 100) : "Backpack", peers: Array.isArray(r.peers) ? r.peers.slice(0, LIMITS.nodes) : [] };
    if (repo.index) {
      try { repo.index = validateIndex(repo.index, repo.key); }
      catch (e: any) { repo.index = undefined; repo.loadedCid = undefined; repo.error = String(e?.message ?? e); }
    }
    out.push(repo);
  }
  return out;
}
export async function saveRepos(r: Repo[]) { await AsyncStorage.setItem(KEY, JSON.stringify(r)); }

/**
 * Ask every pointer source at once — Logos Delivery (ADR 0007: live or from Store, already verified on
 * arrival) and each HTTPS URL — and return the valid pointer with the highest seq, which source gave
 * it, and what every source said (shown in the app, so it's clear where an update came from).
 */
export async function newestPointer(repo: Repo): Promise<{ best: Pointer | null; via?: string; sources: Source[] }> {
  const sources: Source[] = [];
  const cands: { p: Pointer; name: string }[] = [];
  const d = delivery.pointerFor(repo.key), ds = delivery.status();
  if (d) { const name = `Delivery (${delivery.arrivedVia(repo.key) ?? "live"})`; sources.push({ name, seq: d.seq }); cands.push({ p: d, name }); }
  else sources.push({ name: "Delivery", error: ds.state === "on" ? "nothing yet" : ds.state === "unavailable" ? "Loam not available" : "starting" });
  const urls = uniq([...(repo.pointers ?? []), ...(repo.index?.pointers ?? [])].map(safePointerUrl).filter((u): u is string => !!u)).slice(0, LIMITS.pointers);
  if (!settings.get().webPointers) { if (urls.length) sources.push({ name: "web", error: "off in Settings" }); }
  else await Promise.all(urls.map(async (url) => {
    const name = (() => { try { return new URL(url).host; } catch { return url; } })();
    try {
      const ctl = new AbortController(); const tm = setTimeout(() => ctl.abort(), 15000);
      const r = await fetch(url, { signal: ctl.signal, headers: { "Cache-Control": "no-cache" } }); clearTimeout(tm);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      if (r.url && r.url !== url && !safePointerUrl(r.url)) throw new Error("redirected to a non-public address: refusing");   // R-3
      const text = await r.text();
      if (text.length > 8192) throw new Error("too large for a pointer");
      const p = validatePointer(openEnvelope(JSON.parse(text), repo.key, "pointer"), repo.key);
      storage.diag(`pointer ${name}: seq ${p.seq}`);
      sources.push({ name, seq: p.seq }); cands.push({ p, name });
    } catch (e: any) {
      const msg = e?.name === "AbortError" ? "timed out" : String(e?.message ?? e);
      storage.diag(`pointer ${name}: ${msg}`); sources.push({ name, error: msg });
    }
  }));
  let top: { p: Pointer; name: string } | null = null;
  for (const c of cands) if (!top || c.p.seq > top.p.seq) top = c;
  return { best: top?.p ?? null, via: top?.name, sources };
}

/**
 * Bring a repo up to date. Follows the newest signed pointer when it is ahead of what we have;
 * otherwise (re)loads `repo.indexCid` only if we have no index yet. Refuses any index whose seq is
 * below the highest one accepted (rollback), and an index that disagrees with its pointer.
 */
export async function refresh(repo: Repo, onStage?: (s: string) => void, onProgress?: (p: storage.Progress) => void): Promise<Repo> {
  const have = repo.seq ?? 0;
  onStage?.("checking for updates");
  const { best: ptr, via, sources } = await newestPointer(repo);
  let cid = repo.indexCid, expectSeq: number | null = null;
  if (ptr && ptr.seq > have) { cid = ptr.index; expectSeq = ptr.seq; }
  else if (repo.index && cid === repo.loadedCid) {
    return { ...repo, sources, checkedAt: new Date().toISOString(), error: undefined };   // nothing newer: no Storage traffic
  }
  const peers = uniq([...(ptr?.nodes ?? []), ...repo.peers].map((p) => JSON.stringify(p))).map((s) => JSON.parse(s)).slice(0, LIMITS.nodes);
  if (!isCid(cid)) throw new Error("the index CID is malformed");
  const dir = await storage.filesDir();
  const raw = await storage.getFile(cid, `${dir}/indexes/${cid}.json`, peers, onStage, 0, onProgress, { max: LIMITS.indexBytes });
  if (!raw) throw new Error("the index is empty or larger than 1 MB: refusing");
  const index = validateIndex(openEnvelope(JSON.parse(raw), repo.key, "index"), repo.key);
  const seq = index.seq;
  if (seq < have) throw new Error(`this index is older than the one you have (seq ${seq} < ${have}): refusing`);
  if (expectSeq !== null && seq !== expectSeq) throw new Error(`index seq ${seq} doesn't match its pointer (${expectSeq})`);
  const now = new Date().toISOString();
  return {
    ...repo, name: index.name || repo.name, index, indexCid: cid, seq,
    peers: index.nodes.length ? index.nodes : repo.peers,
    pointers: uniq([...(repo.pointers ?? []), ...index.pointers]).slice(0, LIMITS.pointers),
    loadedCid: cid, fetchedAt: now, checkedAt: now, error: undefined, sources,
    via: expectSeq !== null ? via : repo.via ?? "link",
  };
}

/**
 * Add a repo from a link, or bring a known repo (same key) forward. A link to an older index is not an
 * error. For a repo we already have, nothing unsigned from the link is merged in (its nodes and web
 * addresses come only from the signed index): a link can't redirect a trusted repo (audit BP-9).
 */
export async function addFromLink(link: string, repos: Repo[], onStage?: (s: string) => void): Promise<Repo[]> {
  const r = parseLink(link);
  const known = repos.find((x) => x.key === r.key);
  // the link's index CID IS used (audit R-1): the index is signed and its seq can't go below ours, so a
  // newer link moves the repo forward and an older one is refused; only unsigned extras are ignored
  const merged: Repo = known ? { ...known, indexCid: r.indexCid } : r;
  let next: Repo;
  try { next = await refresh(merged, onStage); }
  catch (e: any) {
    if (known && /older than the one you have/.test(String(e?.message))) next = { ...known, pointers: merged.pointers };  // keep the newer one
    else throw e;
  }
  const out = [...repos.filter((x) => x.key !== r.key), next];
  await saveRepos(out);
  return out;
}

export async function iconPath(repo: Repo, app: App): Promise<string | null> {
  if (!app.icon || !isCid(app.icon)) return null;
  const path = `${await storage.filesDir()}/icons/${app.icon}.img`;
  if ((await storage.fileSize(path)) > 0) return path;
  await storage.getFile(app.icon, path, repo.peers, undefined, 0, undefined, { max: LIMITS.iconBytes });   // capped before download (R-5)
  return path;
}

export const installer = {
  available: () => !!Installer,
  canInstall: (): Promise<boolean> => Installer.canInstall(),
  openSettings: (): Promise<boolean> => Installer.openInstallSettings(),
  installedVersion: async (pkg: string): Promise<number> => Number(await Installer.installedVersion(pkg)),
  launch: (pkg: string): Promise<boolean> => Installer.launch(pkg),
  uninstall: (pkg: string): Promise<boolean> => Installer.uninstall(pkg),
  installerOf: (pkg: string): Promise<string> => Installer.installerOf(pkg),
  installedSigners: async (pkg: string): Promise<string[]> => JSON.parse(await Installer.installedSigners(pkg)),
  apkInfo: async (path: string): Promise<{ packageName: string; versionCode: number }> => JSON.parse(await Installer.apkInfo(path)),
};

export interface Update { repo: Repo; app: App; v: Version; from: number }

// Versions the user cancelled while they downloaded automatically: not started again automatically
// (the Update button still works). Keyed by package, newest skipped versionCode.
const SKIP = "backpack.skipped.v1";
export async function skipVersion(pkg: string, versionCode: number) {
  const m = JSON.parse((await AsyncStorage.getItem(SKIP)) ?? "{}"); m[pkg] = Math.max(m[pkg] ?? 0, versionCode);
  await AsyncStorage.setItem(SKIP, JSON.stringify(m));
}
async function skipped(): Promise<Record<string, number>> { try { return JSON.parse((await AsyncStorage.getItem(SKIP)) ?? "{}"); } catch { return {}; } }

/**
 * Updates available for installed apps: across all repos, the newest version of each installed
 * package that is signed by a key the installed app already has (anything else Android would refuse,
 * so it is not offered as an update). Same package in several repos: the highest versionCode wins.
 */
export async function pendingUpdates(repos: Repo[], installed: Record<string, number>): Promise<Update[]> {
  const best = new Map<string, Update>();
  const skip = await skipped();
  for (const repo of repos) for (const app of repo.index?.apps ?? []) {
    const have = installed[app.id] ?? -1, v = app.versions[0];
    if (have < 0 || !v || v.versionCode <= have || !v.signer || (skip[app.id] ?? 0) >= v.versionCode) continue;
    const prev = best.get(app.id);
    if (prev && prev.v.versionCode >= v.versionCode) continue;
    if (!(await installer.installedSigners(app.id)).includes(v.signer)) continue;
    best.set(app.id, { repo, app, v, from: have });
  }
  // Backpack last: installing its own update restarts it, which would cut the rest of the queue short.
  return [...best.values()].sort((a, b) => Number(a.app.id === SELF) - Number(b.app.id === SELF));
}
const SELF = "xyz.vpavlin.backpack";

/** Every repo that lists `pkg`, with the signer of its newest version: lets the app warn when two
 *  repos publish the same package under different keys (only one of them can be the real one). */
export function publishersOf(repos: Repo[], pkg: string): { repo: Repo; signer?: string; versionName: string }[] {
  const out: { repo: Repo; signer?: string; versionName: string }[] = [];
  for (const repo of repos) { const a = repo.index?.apps.find((x) => x.id === pkg); if (a?.versions[0]) out.push({ repo, signer: a.versions[0].signer, versionName: a.versions[0].versionName }); }
  return out;
}

/**
 * Fetch an APK by CID and refuse it unless: its SHA-256 is the signed index's; its signing
 * certificate is the one the index pins; and, if the app is installed, the installed app has that
 * signer too (Android would refuse the update anyway, but only after the user said yes).
 */
export async function install(repo: Repo, app: App, v: Version, onStage: (s: string) => void, onProgress?: (p: storage.Progress) => void): Promise<string> {
  // app.id and versionCode passed the schema (package-name pattern, integer), so this stays in apks/ (BP-7)
  const path = `${await storage.filesDir()}/apks/${app.id}_${v.versionCode}.apk`;
  if (!/^[0-9a-f]{64}$/.test(v.signer ?? "")) throw new Error("this version doesn't pin a signing key: refusing to install it");   // BP-6
  const have = await storage.fileSize(path);
  if (have !== v.size) {
    try { await storage.getFile(v.cid, path, repo.peers, onStage, v.size, onProgress, { exact: v.size }); }
    catch (e) { if (storage.isCancelled(e)) { await Installer.deleteFile(path); return "download-cancelled"; } throw e; }
  }
  onStage("checking");
  const sha = await Installer.sha256(path);
  if (sha !== v.sha256) { await Installer.deleteFile(path); throw new Error("the downloaded APK doesn't match the repo's checksum"); }
  const signers: string[] = JSON.parse(await Installer.apkSigners(path));
  if (!signers.includes(v.signer)) { await Installer.deleteFile(path); throw new Error(`the APK is signed by ${signers[0]?.slice(0, 8) ?? "nobody"}…, not by the key the repo pins (${v.signer.slice(0, 8)}…)`); }
  // the APK must BE the app and version the index says (BP-5): otherwise a repo could list "Bank" and
  // install another package, or loop updates with a mislabelled versionCode
  const info = await installer.apkInfo(path);
  if (info.packageName !== app.id || info.versionCode !== v.versionCode) {
    await Installer.deleteFile(path);
    throw new Error(`the APK is ${info.packageName} (versionCode ${info.versionCode}), but the repo says ${app.id} (${v.versionCode}): refusing`);
  }
  const installed: string[] = JSON.parse(await Installer.installedSigners(app.id));
  if (installed.length && !installed.includes(v.signer)) throw new Error(`the installed ${app.label} is signed by a different key (${installed[0].slice(0, 8)}…). Android won't update it; uninstall it first if you trust this repo.`);
  // Ask for the install permission only now: the download doesn't need it, and the checked file stays
  // cached (same size → no re-download), so the next tap goes straight to installing.
  if (!(await Installer.canInstall())) return "needs-permission";
  onStage("installing");
  const res: string = await Installer.install(path, app.id);
  if (res === "installed") await Installer.deleteFile(path);
  return res;
}
