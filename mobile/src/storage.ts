// storage.ts — Logos Storage on the phone: one fetch-only libstorage node (the native module is
// Scala's LogosStorage, shipped in native/logosstorage), and getFile(cid → path) on top of it.
// Adapted from scala-sdk logos-storage.ts.
import { NativeModules } from "react-native";

const LS: any = (NativeModules as any).LogosStorage;
export const available = () => !!LS;

export interface Peer { peerId: string; addrs: string[]; spr?: string }

const isTimeout = (e: any) => /timeout/i.test(String(e?.message ?? e));

let ctx: string | null = null;
let boot: string[] = [];
let queue: Promise<unknown> = Promise.resolve(); // one init/restart at a time

/** Start the node (or reuse it). A node only discovers through the SPRs it started with, so a new
 *  repo's node SPR restarts it with the union. */
export function init(sprs: string[]): Promise<string> {
  if (!LS) return Promise.reject(new Error("Logos Storage isn't in this build (arm64 phones only)"));
  const run = queue.then(async () => {
    const missing = sprs.filter((s) => s && !boot.includes(s));
    if (ctx && missing.length === 0) return ctx;
    const nodes = [...boot, ...missing];
    if (ctx) await shutdown();
    await LS.setup();
    const dir = `${await LS.filesDir()}/storage`;
    const cfg: Record<string, unknown> = { "log-level": "WARN", "data-dir": dir };
    if (nodes.length) cfg["bootstrap-node"] = nodes; else cfg.network = "logos.test";
    const c = (await LS.newNode(JSON.stringify(cfg))) as string;
    if (!c || c === "0") throw new Error("Storage node didn't start");
    // start can outlast the native call's 120 s wait on a slow network; the node keeps starting, so a
    // timeout here isn't fatal — the next call (connect/fetch) waits on a node that is up by then.
    try { await LS.start(c); } catch (e) { if (!isTimeout(e)) throw e; }
    ctx = c; boot = nodes;
    return c;
  });
  queue = run.catch(() => {});
  return run;
}

export async function filesDir(): Promise<string> { return LS.filesDir(); }
export async function fileSize(path: string): Promise<number> { try { return Number(await LS.fileSize(path)); } catch { return -1; } }

async function shutdown() {
  if (!ctx) return;
  const c = ctx; ctx = null; boot = [];
  for (const f of ["stop", "close", "destroy"]) { try { await LS[f](c); } catch { /* best effort */ } }
}

/**
 * Fetch `cid` from the network into `path`. Dials the repo's node directly first (a NAT-ed phone
 * can't rely on discovery), prefetches with retries — each native call gives up after 120 s while
 * the blocks fetched so far stay in the local store — then writes the file from the local store.
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
