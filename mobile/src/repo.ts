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

const Installer: any = (NativeModules as any).BackpackInstaller;

export interface Version { versionCode: number; versionName: string; size: number; sha256: string; signer?: string; cid: string; minSdk: number; abis: string[] }
export interface App { id: string; label: string; summary?: string; description?: string; license?: string; icon: string | null; versions: Version[] }
export interface Index { v: number; name: string; key: string; seq?: number; created: string; nodes: storage.Peer[]; pointers?: string[]; apps: App[] }
export interface Pointer { key: string; seq: number; index: string; created: string; nodes?: storage.Peer[] }
export interface Repo {
  key: string; name: string; indexCid: string; peers: storage.Peer[];
  seq?: number;            // highest seq accepted: never go below it (rollback protection)
  pointers?: string[];     // where to look for newer indexes (link u= + index.pointers)
  index?: Index; loadedCid?: string; fetchedAt?: string; checkedAt?: string; error?: string;
}

const KEY = "backpack.repos.v1";

const uniq = <T,>(xs: T[]) => [...new Set(xs)];

export function parseLink(link: string): Repo {
  const t = link.trim();
  if (!/^backpack:\/\/repo\?/i.test(t)) throw new Error("not a backpack:// repo link");
  const p = new URLSearchParams(t.slice(t.indexOf("?") + 1));
  const i = p.get("i"), k = p.get("k"), peerId = p.get("p");
  if (!i || !k || !peerId) throw new Error("the link is missing the index, key or node");
  return { key: k, name: p.get("n") ?? "Backpack", indexCid: i, peers: [{ peerId, addrs: p.getAll("a"), spr: p.get("s") ?? undefined }], pointers: p.getAll("u"), seq: 0 };
}

export async function loadRepos(): Promise<Repo[]> {
  try { return JSON.parse((await AsyncStorage.getItem(KEY)) ?? "[]"); } catch { return []; }
}
export async function saveRepos(r: Repo[]) { await AsyncStorage.setItem(KEY, JSON.stringify(r)); }

/** Ask every pointer source; return the valid pointer with the highest seq (or null). Bad sources are skipped. */
export async function newestPointer(repo: Repo): Promise<Pointer | null> {
  let best: Pointer | null = null;
  for (const url of uniq([...(repo.pointers ?? []), ...(repo.index?.pointers ?? [])])) {
    try {
      const ctl = new AbortController(); const tm = setTimeout(() => ctl.abort(), 15000);
      const r = await fetch(url, { signal: ctl.signal, headers: { "Cache-Control": "no-cache" } }); clearTimeout(tm);
      if (!r.ok) { storage.diag(`pointer ${url}: HTTP ${r.status}`); continue; }
      const p = openEnvelope<Pointer>(await r.json(), repo.key, "pointer");
      storage.diag(`pointer ${url}: seq ${p.seq}`);
      if (!best || p.seq > best.seq) best = p;
    } catch (e: any) { storage.diag(`pointer ${url}: ${e?.message ?? e}`); }
  }
  return best;
}

/**
 * Bring a repo up to date. Follows the newest signed pointer when it is ahead of what we have;
 * otherwise (re)loads `repo.indexCid` only if we have no index yet. Refuses any index whose seq is
 * below the highest one accepted (rollback), and an index that disagrees with its pointer.
 */
export async function refresh(repo: Repo, onStage?: (s: string) => void): Promise<Repo> {
  const have = repo.seq ?? 0;
  onStage?.("checking for updates");
  const ptr = await newestPointer(repo);
  let cid = repo.indexCid, expectSeq: number | null = null;
  if (ptr && ptr.seq > have) { cid = ptr.index; expectSeq = ptr.seq; }
  else if (repo.index && cid === repo.loadedCid) {
    return { ...repo, checkedAt: new Date().toISOString(), error: undefined };   // nothing newer: no Storage traffic
  }
  const peers = uniq([...(ptr?.nodes ?? []), ...repo.peers].map((p) => JSON.stringify(p))).map((s) => JSON.parse(s));
  const dir = await storage.filesDir();
  const raw = await storage.getFile(cid, `${dir}/indexes/${cid}.json`, peers, onStage);
  const index = openEnvelope<Index>(JSON.parse(raw), repo.key, "index");
  const seq = index.seq ?? 0;
  if (seq < have) throw new Error(`this index is older than the one you have (seq ${seq} < ${have}): refusing`);
  if (expectSeq !== null && seq !== expectSeq) throw new Error(`index seq ${seq} doesn't match its pointer (${expectSeq})`);
  const now = new Date().toISOString();
  return {
    ...repo, name: index.name || repo.name, index, indexCid: cid, seq,
    peers: index.nodes?.length ? index.nodes : repo.peers,
    pointers: uniq([...(repo.pointers ?? []), ...(index.pointers ?? [])]),
    loadedCid: cid, fetchedAt: now, checkedAt: now, error: undefined,
  };
}

/** Add a repo from a link, or bring a known repo (same key) forward. A link to an older index is not an error. */
export async function addFromLink(link: string, repos: Repo[], onStage?: (s: string) => void): Promise<Repo[]> {
  const r = parseLink(link);
  const known = repos.find((x) => x.key === r.key);
  const merged: Repo = known
    ? { ...known, indexCid: r.indexCid, peers: [...r.peers, ...known.peers], pointers: uniq([...(r.pointers ?? []), ...(known.pointers ?? [])]) }
    : r;
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
  if (!app.icon) return null;
  const path = `${await storage.filesDir()}/icons/${app.icon}.img`;
  if ((await storage.fileSize(path)) > 0) return path;
  await storage.getFile(app.icon, path, repo.peers);
  return path;
}

export const installer = {
  available: () => !!Installer,
  canInstall: (): Promise<boolean> => Installer.canInstall(),
  openSettings: (): Promise<boolean> => Installer.openInstallSettings(),
  installedVersion: async (pkg: string): Promise<number> => Number(await Installer.installedVersion(pkg)),
  launch: (pkg: string): Promise<boolean> => Installer.launch(pkg),
};

/**
 * Fetch an APK by CID and refuse it unless: its SHA-256 is the signed index's; its signing
 * certificate is the one the index pins; and, if the app is installed, the installed app has that
 * signer too (Android would refuse the update anyway, but only after the user said yes).
 */
export async function install(repo: Repo, app: App, v: Version, onStage: (s: string) => void): Promise<string> {
  const path = `${await storage.filesDir()}/apks/${app.id}_${v.versionCode}.apk`;
  const have = await storage.fileSize(path);
  if (have !== v.size) await storage.getFile(v.cid, path, repo.peers, onStage);
  onStage("checking");
  const sha = await Installer.sha256(path);
  if (sha !== v.sha256) { await Installer.deleteFile(path); throw new Error("the downloaded APK doesn't match the repo's checksum"); }
  if (v.signer) {
    const signers: string[] = JSON.parse(await Installer.apkSigners(path));
    if (!signers.includes(v.signer)) { await Installer.deleteFile(path); throw new Error(`the APK is signed by ${signers[0]?.slice(0, 8) ?? "nobody"}…, not by the key the repo pins (${v.signer.slice(0, 8)}…)`); }
    const installed: string[] = JSON.parse(await Installer.installedSigners(app.id));
    if (installed.length && !installed.includes(v.signer)) throw new Error(`the installed ${app.label} is signed by a different key (${installed[0].slice(0, 8)}…). Android won't update it; uninstall it first if you trust this repo.`);
  }
  onStage("installing");
  const res: string = await Installer.install(path);
  if (res === "installed") await Installer.deleteFile(path);
  return res;
}
