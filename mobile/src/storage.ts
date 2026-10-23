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
    diag("init: setup");
    await LS.setup();
    // Storage is only a cache here, and a data dir left by a download that was cut short (app killed,
    // crash) makes the next download hang (reproduced on desktop with the same libstorage commit).
    // So every process starts from an empty one.
    const dir = `${await LS.filesDir()}/storage`;
    await (NativeModules as any).BackpackInstaller?.deleteTree(dir);
    const nodes = sprs.filter(Boolean);
    const cfg: Record<string, unknown> = { "log-level": "WARN", "data-dir": dir };
    if (nodes.length) cfg["bootstrap-node"] = nodes; else cfg.network = "logos.test";
    diag(`init: newNode (bootstrap ${nodes.length})`);
    const c = (await LS.newNode(JSON.stringify(cfg))) as string;
    if (!c || c === "0") throw new Error("Storage node didn't start");
    // A start that fails leaves a node that must never be started again (libstorage aborts on a
    // second start): destroy it, so the next attempt creates a fresh one.
    diag("init: start");
    try { await LS.start(c); diag("init: started"); }
    catch (e) { for (const f of ["stop", "close", "destroy"]) { try { await LS[f](c); } catch { /* */ } } throw e; }
    ctx = c;
    return c;
  });
  queue = run.catch(() => {});
  return run;
}

export async function filesDir(): Promise<string> { return LS.filesDir(); }

// ── diagnostics: what the node is doing, readable while a fetch is stuck ──────
export interface DiagLine { t: number; text: string }
const diagLog: DiagLine[] = [];
const diagListeners = new Set<() => void>();
const t0 = Date.now();
export function diag(text: string) {
  diagLog.push({ t: Date.now(), text });
  if (diagLog.length > 200) diagLog.shift();
  diagListeners.forEach((l) => l());
}
export function diagLines(): DiagLine[] { return [...diagLog]; }
export function onDiag(l: () => void): () => void { diagListeners.add(l); return () => diagListeners.delete(l); }
export function diagText(): string {
  return diagLog.map((d) => `+${((d.t - t0) / 1000).toFixed(1)}s ${d.text}`).join("\n");
}
/** The node's own view (peers, addresses, table) — runs on a separate native thread, never blocked by a fetch. */
export async function nodeDebug(): Promise<string> {
  if (!ctx || !NATIVE) return "node not started";
  try { return await NATIVE.debug(ctx); } catch (e: any) { return "debug failed: " + (e?.message ?? e); }
}
/** One-line summary of nodeDebug: own id, addresses, how many peers the DHT table knows. */
export async function nodeSummary(): Promise<string> {
  const raw = await nodeDebug();
  try {
    const j = JSON.parse(raw);
    const n = j?.table?.nodes?.length ?? "?";
    return `node ${String(j.id ?? "").slice(-8)} addrs ${JSON.stringify(j.addrs ?? [])} table ${n}`;
  } catch { return raw.slice(0, 300); }
}
export async function dirSize(path: string): Promise<number> { try { return Number(await LS.dirSize(path)); } catch { return 0; } }
export async function fileSize(path: string): Promise<number> { try { return Number(await LS.fileSize(path)); } catch { return -1; } }


/**
 * Fetch `cid` from the network into `path`. Dials the repo's node directly first (a NAT-ed phone
 * can't rely on discovery), prefetches (each native call waits up to 10 min; a timeout is retried while
 * the blocks fetched so far stay in the local store), then writes the file.
 * Returns the content when it is small (≤ 1 MB: the index), else "".
 */
export type Progress = { phase: "downloading" | "saving"; bytes: number; total: number; rate: number };

/** Poll `measure` every second while `work` runs; report bytes, total and a smoothed rate. Logs every 15 s. */
async function watch<T>(work: Promise<T>, measure: () => Promise<number>, phase: Progress["phase"], total: number, onProgress?: (p: Progress) => void): Promise<T> {
  if (!onProgress) return work;
  let last = Math.max(0, await measure()), lastT = Date.now(), rate = 0, ticks = 0, done = false;
  const tick = async () => {
    while (!done) {
      await new Promise((r) => setTimeout(r, 1000));
      if (done) break;
      const b = Math.max(last, await measure()), t = Date.now();
      rate = rate ? 0.7 * rate + 0.3 * ((b - last) * 1000 / Math.max(1, t - lastT)) : (b - last) * 1000 / Math.max(1, t - lastT);
      last = b; lastT = t;
      onProgress({ phase, bytes: total ? Math.min(b, total) : b, total, rate: Math.max(0, rate) });
      if (++ticks % 15 === 0) diag(`${phase} ${(b / 1048576).toFixed(1)}${total ? `/${(total / 1048576).toFixed(1)}` : ""} MB · ${(Math.max(0, rate) / 1024).toFixed(0)} KB/s`);
    }
  };
  onProgress({ phase, bytes: total ? Math.min(last, total) : last, total, rate: 0 });
  tick();
  try { return await work; } finally { done = true; }
}

// ── cancelling a download ────────────────────────────────────────────────────
// The download runs inside one blocking native call on the storage thread; cancel() asks libstorage
// to drop it (storage_download_cancel, on another thread), which makes that call return with an error.
// The flag stops getFile's retries and turns the error into CANCELLED.
const cancelled = new Set<string>();
export const CANCELLED = "download cancelled";
export const isCancelled = (e: unknown) => String((e as any)?.message ?? e) === CANCELLED;
export async function cancel(cid: string): Promise<void> {
  cancelled.add(cid);
  diag(`cancel ${cid.slice(-10)}`);
  if (ctx) { try { await LS.downloadCancel(ctx, cid); } catch (e: any) { diag(`cancel: ${e?.message ?? e}`); } }
}
function checkCancel(cid: string) { if (cancelled.has(cid)) throw new Error(CANCELLED); }
async function dropBlocks(c: string, cid: string) { try { await LS.delete(c, cid); } catch { /* best effort: the data dir is wiped at launch anyway */ } }

export async function getFile(cid: string, path: string, peers: Peer[], onStage?: (s: string) => void, total = 0, onProgress?: (p: Progress) => void): Promise<string> {
  const c = await init(peers.map((p) => p.spr ?? "").filter(Boolean));
  try { const r = await getFileOn(c, cid, path, peers, onStage, total, onProgress); cancelled.delete(cid); return r; }
  catch (e) {
    if (!cancelled.has(cid)) throw e;
    cancelled.delete(cid); dropBlocks(c, cid);
    throw new Error(CANCELLED);
  }
}

async function getFileOn(c: string, cid: string, path: string, peers: Peer[], onStage: ((s: string) => void) | undefined, total: number, onProgress?: (p: Progress) => void): Promise<string> {
  // One progress measure across both calls: blocks landing in the node's store (fetch) or the file
  // growing (download_stream), whichever is further. Never goes backwards.
  const store = `${await LS.filesDir()}/storage`, base = await dirSize(store);
  const measure = async () => Math.max((await dirSize(store)) - base, await fileSize(path));
  for (const p of peers) {
    checkCancel(cid);
    try { onStage?.("connecting"); diag(`connect ${p.peerId.slice(-8)} ${p.addrs.join(",")}`); await LS.connect(c, p.peerId, p.addrs); diag("connect ok"); }
    catch (e: any) { diag(`connect FAILED: ${e?.message ?? e}`); }
  }
  // fetch pulls the blocks into the local store; download_stream alone (local=false) only gets the
  // manifest and then waits forever (verified on desktop). Scala's prebuilt arm64 libstorage crashed
  // inside fetch (SEGV on the phone and under arm64 translation); the library is now built fresh.
  for (let i = 1; ; i++) {
    checkCancel(cid);
    try { onStage?.(i === 1 ? "fetching" : `fetching (try ${i})`); diag(`fetch ${cid.slice(-10)} try ${i}`); const m = await watch(LS.fetch(c, cid), measure, "downloading", total, onProgress); diag(`fetch ok ${String(m).slice(0, 120)}`); break; }
    catch (e: any) { diag(`fetch error: ${e?.message ?? e}`); checkCancel(cid); if (!isTimeout(e) || i >= 3) throw e; }
  }
  for (let i = 1; ; i++) {
    checkCancel(cid);
    try { onStage?.(i === 1 ? "writing" : `writing (try ${i})`); diag(`download ${cid.slice(-10)} try ${i}`); const r = await watch<string>(LS.downloadToFile(c, cid, 65536, false, path), measure, "saving", total, onProgress); diag(`download ok (${await fileSize(path)} B)`); return r; }
    catch (e: any) { diag(`download error: ${e?.message ?? e}`); checkCancel(cid); if (!isTimeout(e) || i >= 5) throw e; }
  }
}
