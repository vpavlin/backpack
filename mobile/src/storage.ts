// storage.ts — Logos Storage on the phone: one fetch-only libstorage node (the native module is
// Scala's LogosStorage, shipped in native/logosstorage), and getFile(cid → path) on top of it.
// Adapted from scala-sdk logos-storage.ts.
import { NativeModules } from "react-native";
import * as loam from "./lib/loam-transport-pkg/src/loam-storage";
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
class SizeError extends Error {}
// fetch resolves with the CID's manifest; its datasetSize is the file's size, known before any data block
function checkSize(manifest: unknown, rule: SizeRule) {
  if (rule.exact === undefined && rule.max === undefined) return;
  const m = /"datasetSize"\s*:\s*(\d+)/.exec(String(manifest ?? ""));
  if (!m) throw new SizeError("the file's size is unknown: refusing");
  const n = Number(m[1]);
  if (rule.exact !== undefined && n !== rule.exact) throw new SizeError(`the file is ${n} bytes, the repo says ${rule.exact}: refusing`);
  if (rule.max !== undefined && n > rule.max) throw new SizeError(`the file is ${n} bytes, over the ${rule.max}-byte limit: refusing`);
}
export const isCancelled = (e: unknown) => String((e as any)?.message ?? e) === CANCELLED;
export async function cancel(cid: string): Promise<void> {
  cancelled.add(cid);
  diag(`cancel ${cid.slice(-10)}`);
  if (viaLoam.has(cid)) { try { await loam.cancel(cid); } catch { /* */ } }
  if (ctx) { try { await LS.downloadCancel(ctx, cid); } catch (e: any) { diag(`cancel: ${e?.message ?? e}`); } }
}
function checkCancel(cid: string) { if (cancelled.has(cid)) throw new Error(CANCELLED); }
async function dropBlocks(c: string, cid: string) { try { await LS.delete(c, cid); } catch { /* best effort: the data dir is wiped at launch anyway */ } }

/** Size rules checked against the manifest BEFORE the download (audit R-5/BP-22): `exact` for an APK
 *  (the signed index says its size), `max` for an index or icon. */
export interface SizeRule { exact?: number; max?: number }

export async function getFile(cid: string, path: string, peers: Peer[], onStage?: (s: string) => void, total = 0, onProgress?: (p: Progress) => void, size: SizeRule = {}): Promise<string> {
  // Through Loam when it has Storage (loam-transport ADR 0020): Loam fetches in the background, so the
  // download survives this app being closed, and one cache serves every app. Else our own node.
  if (await loamAvailable()) {
    try { return await getViaLoam(cid, path, peers, onStage, onProgress, size); }
    catch (e: any) {
      // Loam caps one file at 512 MiB; an encrypted file is a little bigger than its APK (audit PR-7).
      // That one refusal falls back to our own node; any other error stands.
      if (!/too large/i.test(String(e?.message ?? e))) throw e;
      diag(`loam refused ${cid.slice(-10)} as too large: using our own node`);
    }
    finally { viaLoam.delete(cid); }
  }
  const c = await init(peers.map((p) => p.spr ?? "").filter(Boolean));
  try { const r = await getFileOn(c, cid, path, peers, onStage, total, onProgress, size); cancelled.delete(cid); return r; }
  catch (e) {
    if (!cancelled.has(cid)) throw e;
    cancelled.delete(cid); dropBlocks(c, cid);
    throw new Error(CANCELLED);
  }
}

async function getFileOn(c: string, cid: string, path: string, peers: Peer[], onStage: ((s: string) => void) | undefined, total: number, onProgress: ((p: Progress) => void) | undefined, size: SizeRule): Promise<string> {
  // One progress measure across both calls: blocks landing in the node's store (fetch) or the file
  // growing (download_stream), whichever is further. Never goes backwards.
  const store = `${await LS.filesDir()}/storage`, base = await dirSize(store);
  // Progress = bytes of THIS file written so far. The block store's growth looked tempting but it also
  // grows from other fetches still running in the background (icons, earlier CIDs), so it ran ahead of
  // the truth ("30 of 40 MB" while still pulling). download_stream writes the file as blocks arrive.
  void store; void base;
  const measure = async () => Math.max(0, await fileSize(path));
  for (const p of peers) {
    checkCancel(cid);
    try { onStage?.("connecting"); diag(`connect ${p.peerId.slice(-8)} ${p.addrs.join(",")}`); await LS.connect(c, p.peerId, p.addrs); diag("connect ok"); }
    catch (e: any) { diag(`connect FAILED: ${e?.message ?? e}`); }
  }
  // size BEFORE any data block (audit R-5): storage_download_manifest fetches only the manifest
  if (size.exact !== undefined || size.max !== undefined) {
    checkCancel(cid);
    onStage?.("checking size");
    let m: string;
    try { m = await LS.manifest(c, cid); diag(`manifest ${String(m).slice(0, 160)}`); }
    catch (e: any) { diag(`manifest error: ${e?.message ?? e}`); checkCancel(cid); throw e; }
    checkSize(m, size);
  }
  // fetch pulls the blocks into the local store; download_stream alone (local=false) only gets the
  // manifest and then waits forever (verified on desktop). Scala's prebuilt arm64 libstorage crashed
  // inside fetch (SEGV on the phone and under arm64 translation); the library is now built fresh.
  for (let i = 1; ; i++) {
    checkCancel(cid);
    try { onStage?.(i === 1 ? "fetching" : `fetching (try ${i})`); diag(`fetch ${cid.slice(-10)} try ${i}`); const m = await watch(LS.fetch(c, cid), measure, "downloading", total, onProgress); diag(`fetch ok ${String(m).slice(0, 120)}`); break; }
    catch (e: any) {
      diag(`fetch error: ${e?.message ?? e}`); checkCancel(cid);
      if (!isTimeout(e) || i >= 2) throw e;   // fewer retries: the storage thread is shared (R-11)
    }
  }
  for (let i = 1; ; i++) {
    checkCancel(cid);
    try { onStage?.(i === 1 ? "writing" : `writing (try ${i})`); diag(`download ${cid.slice(-10)} try ${i}`); const r = await watch<string>(LS.downloadToFile(c, cid, 65536, false, path), measure, "saving", total, onProgress); diag(`download ok (${await fileSize(path)} B)`); return r; }
    catch (e: any) { diag(`download error: ${e?.message ?? e}`); checkCancel(cid); if (!isTimeout(e) || i >= 3) throw e; }
  }
}

// ── Logos Storage through Loam ───────────────────────────────────────────────────────────────────
const viaLoam = new Set<string>();
let loamChecked = 0, loamOk = false;
/** Re-checked at most every 30 s: Loam may be installed, approved or updated while we run. */
async function loamAvailable(): Promise<boolean> {
  if (Date.now() - loamChecked < 30_000) return loamOk;
  loamChecked = Date.now();
  try { loamOk = await loam.available(); } catch { loamOk = false; }
  diag(`storage via Loam: ${loamOk ? "yes" : "no (own node)"}`);
  return loamOk;
}

async function getViaLoam(cid: string, path: string, peers: Peer[], onStage: ((s: string) => void) | undefined,
                          onProgress: ((p: Progress) => void) | undefined, size: SizeRule, retried = false): Promise<string> {
  viaLoam.add(cid);
  onStage?.("fetching");
  let st = await loam.fetch(cid, peers.slice(0, 8));
  if (!st) throw new Error("Loam didn't answer");
  diag(`loam job ${cid.slice(-10)}: ${st.state}`);
  let last = 0, lastT = Date.now(), rate = 0, checkedSize = false, restarts = 0;
  let seen = "", changedAt = Date.now();   // older Loams have no stall watchdog: give up after 10 min without change
  while (st && (st.state === "queued" || st.state === "fetching")) {
    checkCancel(cid);
    // the size comes from the manifest (Loam reads it before any data block): check it as soon as known
    if (!checkedSize && (st.total ?? 0) > 0) {
      checkedSize = true;
      try { checkSize(`"datasetSize":${st.total}`, size); }
      catch (e) { await loam.cancel(cid); throw e; }
    }
    const b = st.bytes ?? 0, t = Date.now();
    rate = 0.7 * rate + 0.3 * ((b - last) * 1000 / Math.max(1, t - lastT)); last = b; lastT = t;
    onProgress?.({ phase: "downloading", bytes: b, total: st.total ?? 0, rate: Math.max(0, rate) });
    const now = `${st.state}/${b}`;
    if (now !== seen) { seen = now; changedAt = t; }
    else if ((st as any).idleMs === undefined && t - changedAt > 600_000) { await loam.cancel(cid); throw new Error("Loam: no progress for 10 min"); }
    await new Promise((r) => setTimeout(r, 1000));
    try { st = await loam.status(cid); } catch { st = null; }
    // "none": Loam forgot the job (cache cleared, file evicted, Loam restarted); no answer: Loam was
    // restarted or killed. "failed: stalled": Loam's job got no data for minutes (network changed). Ask
    // again, a few times.
    const lost = !st || st.state === "none" || (st.state === "failed" && /^stalled/.test(st.error ?? ""));
    if (lost && restarts++ < 3) {
      diag(`loam job ${cid.slice(-10)} ${st ? st.state + (st.error ? ` (${st.error})` : "") : "no answer"}: fetching again`);
      await new Promise((r) => setTimeout(r, 2000));
      try { st = await loam.fetch(cid, peers.slice(0, 8)); } catch { st = null; }
    }
  }
  checkCancel(cid);
  if (!st || st.state !== "done") {
    if (st?.state === "cancelled") throw new Error(CANCELLED);
    throw new Error(`Loam: ${st?.error ?? st?.state ?? "no answer"}`);
  }
  checkSize(`"datasetSize":${st.total ?? st.bytes ?? 0}`, size);
  onStage?.("writing");
  let n: number;
  try { n = await loam.saveTo(cid, path); }
  catch (e) {
    // the file went away between "done" and the copy (cache cleared): one more round
    if (retried) throw e;
    diag(`loam ${cid.slice(-10)}: file gone before the copy, fetching again`);
    viaLoam.delete(cid);
    return getViaLoam(cid, path, peers, onStage, onProgress, size, true);
  }
  diag(`loam ${cid.slice(-10)}: ${n} bytes`);
  // return the content when small (the index), like the embedded path
  return n > 0 && n <= (1 << 20) ? String(await LS.readFileUtf8(path)) : "";
}
