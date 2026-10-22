// repo.ts — Backpack repositories: parse a repo link, fetch + verify the signed index from Logos
// Storage, remember repos, and fetch APKs/icons by CID.
//
// Link: backpack://repo?i=<indexCID>&k=<ed25519 pubkey, b64url>&n=<name>&p=<peerId>&a=<addr>…&s=<spr>
// Index file (a Storage object): {"backpack":1,"body":"<json>","sig":"<b64url ed25519 over body>"}
// A repo is identified by its key: a later index replaces an earlier one only if the same key
// signed it and it is newer.
import AsyncStorage from "@react-native-async-storage/async-storage";
import { ed25519 } from "@noble/curves/ed25519";
import { NativeModules } from "react-native";
import * as storage from "./storage";

const Installer: any = (NativeModules as any).BackpackInstaller;

export interface Version { versionCode: number; versionName: string; size: number; sha256: string; cid: string; minSdk: number; abis: string[] }
export interface App { id: string; label: string; icon: string | null; versions: Version[] }
export interface Index { v: number; name: string; key: string; created: string; nodes: storage.Peer[]; apps: App[] }
export interface Repo { key: string; name: string; indexCid: string; peers: storage.Peer[]; index?: Index; fetchedAt?: string; error?: string }

const KEY = "backpack.repos.v1";

const b64urlToBytes = (s: string) => {
  const b = s.replace(/-/g, "+").replace(/_/g, "/");
  const bin = globalThis.atob(b + "===".slice((b.length + 3) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
};
const utf8 = (s: string) => new TextEncoder().encode(s);

export function parseLink(link: string): Repo {
  const t = link.trim();
  const q = t.slice(t.indexOf("?") + 1);
  if (!/^backpack:\/\/repo\?/i.test(t)) throw new Error("not a backpack:// repo link");
  const p = new URLSearchParams(q);
  const i = p.get("i"), k = p.get("k"), peerId = p.get("p");
  if (!i || !k || !peerId) throw new Error("the link is missing the index, key or node");
  return { key: k, name: p.get("n") ?? "Backpack", indexCid: i, peers: [{ peerId, addrs: p.getAll("a"), spr: p.get("s") ?? undefined }] };
}

export async function loadRepos(): Promise<Repo[]> {
  try { return JSON.parse((await AsyncStorage.getItem(KEY)) ?? "[]"); } catch { return []; }
}
export async function saveRepos(r: Repo[]) { await AsyncStorage.setItem(KEY, JSON.stringify(r)); }

/** Fetch the repo's index from Storage and verify it. Returns the repo with the index filled in. */
export async function refresh(repo: Repo, onStage?: (s: string) => void): Promise<Repo> {
  const dir = await storage.filesDir();
  const raw = await storage.getFile(repo.indexCid, `${dir}/indexes/${repo.indexCid}.json`, repo.peers, onStage);
  const file = JSON.parse(raw);
  if (file?.backpack !== 1 || typeof file.body !== "string" || typeof file.sig !== "string") throw new Error("not a Backpack index");
  if (!ed25519.verify(b64urlToBytes(file.sig), utf8(file.body), b64urlToBytes(repo.key))) throw new Error("index signature doesn't match the repo key");
  const index = JSON.parse(file.body) as Index;
  if (index.key !== repo.key) throw new Error("index was signed for a different repo");
  if (repo.index && index.created < repo.index.created) throw new Error("this index is older than the one you have");
  // the index names the nodes holding the repo (with SPRs): use them from now on
  const peers = index.nodes?.length ? index.nodes : repo.peers;
  return { ...repo, name: index.name || repo.name, index, peers, fetchedAt: new Date().toISOString(), error: undefined };
}

/** Add a repo from a link, or move a known repo (same key) to a newer index. */
export async function addFromLink(link: string, repos: Repo[], onStage?: (s: string) => void): Promise<Repo[]> {
  const r = parseLink(link);
  const known = repos.find((x) => x.key === r.key);
  const next = await refresh(known ? { ...known, indexCid: r.indexCid, peers: [...r.peers, ...known.peers] } : r, onStage);
  const out = [...repos.filter((x) => x.key !== r.key), next];
  await saveRepos(out);
  return out;
}

export async function iconPath(repo: Repo, app: App): Promise<string | null> {
  if (!app.icon) return null;
  const path = `${await storage.filesDir()}/icons/${app.icon}.png`;
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

/** Fetch an APK by CID, check its SHA-256 against the signed index, and hand it to Android. */
export async function install(repo: Repo, app: App, v: Version, onStage: (s: string) => void): Promise<string> {
  const path = `${await storage.filesDir()}/apks/${app.id}_${v.versionCode}.apk`;
  const have = await storage.fileSize(path);
  if (have !== v.size) await storage.getFile(v.cid, path, repo.peers, onStage);
  onStage("checking");
  const sha = await Installer.sha256(path);
  if (sha !== v.sha256) { await Installer.deleteFile(path); throw new Error("the downloaded APK doesn't match the repo's checksum"); }
  onStage("installing");
  const res: string = await Installer.install(path);
  if (res === "installed") await Installer.deleteFile(path);
  return res;
}
