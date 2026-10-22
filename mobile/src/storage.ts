// storage.ts — Logos Storage on the phone: one fetch-only libstorage node (the native module is
// Scala's LogosStorage, shipped in native/logosstorage), and getFile(cid → path) on top of it.
// Adapted from scala-sdk logos-storage.ts.
import { NativeModules } from "react-native";
import AsyncStorage from "@react-native-async-storage/async-storage";

const NATIVE: any = (NativeModules as any).LogosStorage;

// Crash breadcrumb: before every native Storage call we persist which call it is, and clear it when
// the call returns. A native crash kills the app mid-call; on the next launch lastCrash() names it.
const CRUMB = "backpack.native-step";
let calls = 0;
export async function lastCrash(): Promise<string | null> {
  const v = await AsyncStorage.getItem(CRUMB);
  if (v) await AsyncStorage.removeItem(CRUMB);
  return v;
}
const LS: any = NATIVE && new Proxy(NATIVE, {
  get(target, name: string) {
    const f = target[name];
    if (typeof f !== "function") return f;
    return async (...a: any[]) => {
      const mark = `${name} (${new Date().toISOString()}, call ${++calls})`;
      await AsyncStorage.setItem(CRUMB, mark);
      try { return await f(...a); }
      finally { if ((await AsyncStorage.getItem(CRUMB)) === mark) await AsyncStorage.removeItem(CRUMB); } // only our own mark
    };
  },
});
export const available = () => !!LS;

export interface Peer { peerId: string; addrs: string[]; spr?: string }

const isTimeout = (e: any) => /timeout/i.test(String(e?.message ?? e));

let ctx: string | null = null;
let queue: Promise<unknown> = Promise.resolve(); // one init/restart at a time

/** Start the node once per process. Bootstrap records only feed discovery: every repo node is also
 *  dialled directly (getFile → connect), so a node learned later needs no restart. (Restarting
 *  libstorage in-process hung the next download, and starting a node twice aborts it.) */
export function init(sprs: string[]): Promise<string> {
  if (!LS) return Promise.reject(new Error("Logos Storage isn't in this build (arm64 phones only)"));
  const run = queue.then(async () => {
    if (ctx) return ctx;
    await LS.setup();
    // Storage is only a cache here, and a data dir left by a download that was cut short (app killed,
    // crash) makes the next download hang (reproduced on desktop with the same libstorage commit).
    // So every process starts from an empty one.
    const dir = `${await LS.filesDir()}/storage`;
    await (NativeModules as any).BackpackInstaller?.deleteTree(dir);
    const nodes = sprs.filter(Boolean);
    const cfg: Record<string, unknown> = { "log-level": "WARN", "data-dir": dir };
    if (nodes.length) cfg["bootstrap-node"] = nodes; else cfg.network = "logos.test";
    const c = (await LS.newNode(JSON.stringify(cfg))) as string;
    if (!c || c === "0") throw new Error("Storage node didn't start");
    // A start that fails leaves a node that must never be started again (libstorage aborts on a
    // second start): destroy it, so the next attempt creates a fresh one.
    try { await LS.start(c); }
    catch (e) { for (const f of ["stop", "close", "destroy"]) { try { await LS[f](c); } catch { /* */ } } throw e; }
    ctx = c;
    return c;
  });
  queue = run.catch(() => {});
  return run;
}

export async function filesDir(): Promise<string> { return LS.filesDir(); }
export async function fileSize(path: string): Promise<number> { try { return Number(await LS.fileSize(path)); } catch { return -1; } }


/**
 * Fetch `cid` from the network into `path`. Dials the repo's node directly first (a NAT-ed phone
 * can't rely on discovery), prefetches (each native call waits up to 10 min; a timeout is retried while
 * the blocks fetched so far stay in the local store), then writes the file.
 * Returns the content when it is small (≤ 1 MB: the index), else "".
 */
export async function getFile(cid: string, path: string, peers: Peer[], onStage?: (s: string) => void): Promise<string> {
  const c = await init(peers.map((p) => p.spr ?? "").filter(Boolean));
  for (const p of peers) { try { onStage?.("connecting"); await LS.connect(c, p.peerId, p.addrs); } catch { /* try the others */ } }
  for (let i = 1; ; i++) {
    try { onStage?.(i === 1 ? "fetching" : `fetching (try ${i})`); await LS.fetch(c, cid); break; }
    catch (e) { if (!isTimeout(e) || i >= 15) throw e; }
  }
  for (let i = 1; ; i++) {
    try { onStage?.("writing"); return await LS.downloadToFile(c, cid, 65536, false, path); }
    catch (e) { if (!isTimeout(e) || i >= 5) throw e; }
  }
}
