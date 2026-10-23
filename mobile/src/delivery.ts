// delivery.ts — repo pointers over Logos Delivery (ADR 0007), through the Loam app's shared node.
// Each repo's signed pointer is published to /backpack/1/<repo key>/json (and re-sent twice a day so
// Store keeps it). Backpack listens on those topics while open and pulls Store on start; a valid pointer
// with a higher seq triggers the same refresh as one from an HTTPS source. Nothing here is trusted:
// every message is opened with the repo's key (openEnvelope) and only the highest seq counts.
// Without the Loam app, start() fails quietly and the HTTPS sources still work.
import AsyncStorage from "@react-native-async-storage/async-storage";
import * as T from "./lib/loam-transport";
import { openEnvelope } from "./envelope";
import * as storage from "./storage";

export interface Pointer { key: string; seq: number; index: string; created: string; nodes?: storage.Peer[] }

const APP_ID = "xyz.vpavlin.backpack";
export const topicFor = (key: string) => `/backpack/1/${key}/json`;

let state: "off" | "starting" | "on" | "unavailable" = "off";
let lastError = "";
const keys = new Set<string>();
const best = new Map<string, Pointer>();            // repo key → newest valid pointer seen over Delivery
const via = new Map<string, "live" | "store">();    // how that pointer arrived
const heard = new Map<string, number>();            // repo key → when a valid note last arrived (any seq)
let listeningSince = 0;                             // a pointer signed before this came from Store, not live
let onPointer: ((p: Pointer) => void) | null = null;

/** Newest pointer for `key` received over Delivery (live or from Store), if any. */
export function pointerFor(key: string): Pointer | undefined { return best.get(key); }
export function arrivedVia(key: string): "live" | "store" | undefined { return via.get(key); }
export function status(): { state: typeof state; error: string } { return { state, error: lastError }; }

function receive(topic: string, candidates: Uint8Array[]): boolean {
  const key = [...keys].find((k) => topicFor(k) === topic);
  if (!key) return false;
  for (const c of candidates) {
    try {
      const p = openEnvelope<Pointer>(JSON.parse(new TextDecoder().decode(c)), key, "pointer");
      storage.diag(`delivery: pointer ${key.slice(0, 6)} seq ${p.seq}`);
      heard.set(key, Date.now());
      const prev = best.get(key);
      if (!prev || p.seq > prev.seq) { best.set(key, p); via.set(key, Date.parse(p.created) < listeningSince ? "store" : "live"); onPointer?.(p); }
      return true;
    } catch { /* not ours, not valid, or another candidate decoding: try the next */ }
  }
  return false;
}

async function deviceId(): Promise<string> {
  let id = await AsyncStorage.getItem("backpack.deviceId");
  if (!id) { id = `bp-${Math.random().toString(36).slice(2)}${Date.now().toString(36)}`; await AsyncStorage.setItem("backpack.deviceId", id); }
  return id;
}

/** Listen for the given repos' pointers. Safe to call again with more keys (joins the new topics). */
export async function watch(repoKeys: string[], cb: (p: Pointer) => void): Promise<void> {
  onPointer = cb;
  const fresh = repoKeys.filter((k) => !keys.has(k));
  fresh.forEach((k) => keys.add(k));
  if (state === "unavailable" || state === "starting") return;
  try {
    if (state === "on") {
      if (fresh.length) { await T.join(fresh.map(topicFor)); pull(); }
      return;
    }
    state = "starting"; listeningSince = Date.now();
    // must come before anything that wires the transport backend (loam-integrate-app §2)
    T.preferServiceBackend(true, APP_ID);
    await T.start({ deviceId: await deviceId(), topics: [...keys].map(topicFor), onReceive: receive, onStatus: (s) => storage.diag(`delivery: ${s}`) });
    state = "on"; lastError = "";
    storage.diag(`delivery: listening on ${keys.size} repo topic(s) via ${T.usingServiceBackend() ? "Loam" : "own node"}`);
    pull();
  } catch (e: any) {
    state = "unavailable"; lastError = String(e?.message ?? e);
    storage.diag(`delivery: unavailable (${lastError}); using web pointers only`);
  }
}

/** Ask Store for what was published while we weren't listening (the latest pointer per repo). */
export function pull(): void {
  if (state !== "on") return;
  Promise.resolve(T.storeSync(receive))
    .then((r: any) => storage.diag(`delivery: store pull done${r?.msgs !== undefined ? ` (${r.msgs} msgs)` : ""}`))
    .catch((e: any) => storage.diag(`delivery: store pull failed: ${e?.message ?? e}`));
}

/**
 * Pull-to-refresh: ask Store again and wait (up to `ms`) until every given repo's note has come back.
 * On the Loam shared node the Store query is fire-and-forget: Loam runs it and the results arrive
 * through receive(), like live messages. (Loam skips a topic it pulled within the last minute; then
 * the note we already hold is still the newest, and the wait just times out.)
 */
export async function pullAndWait(repoKeys: string[], ms = 8000): Promise<void> {
  const since = Date.now();
  while (state === "starting" && Date.now() - since < ms) await new Promise((r) => setTimeout(r, 250));   // first open: the node is still coming up
  if (state !== "on" || !repoKeys.length) return;
  pull();
  while (Date.now() - since < ms) {
    if (repoKeys.every((k) => (heard.get(k) ?? 0) >= since)) return;
    await new Promise((r) => setTimeout(r, 250));
  }
}
