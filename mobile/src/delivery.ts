// delivery.ts — repo pointers over Logos Delivery (ADR 0007), through the Loam app's shared node.
// Each repo's signed pointer is published to /backpack/1/<repo key>/json (and re-sent twice a day so
// Store keeps it). Backpack listens on those topics while open and pulls Store on start; a valid pointer
// with a higher seq triggers the same refresh as one from an HTTPS source. Nothing here is trusted:
// every message is opened with the repo's key (openEnvelope) and only the highest seq counts.
// The newest note per repo is also SAVED (as received, still signed) and checked again on load: a note
// whose index we failed to fetch (network changed mid-download) is not delivered a second time, so
// without the saved copy Backpack would forget that a newer list exists and never try again.
// Without the Loam app, start() fails quietly and the HTTPS sources still work.
import AsyncStorage from "@react-native-async-storage/async-storage";
import * as T from "./lib/loam-transport";
import { openEnvelope } from "./envelope";
import { topicId } from "./crypt";
import * as storage from "./storage";
import { validatePointer } from "./schema";
import type { Pointer } from "./schema";
export type { Pointer };

// The topics are public: anyone can publish to them. Bound the work per message before parsing
// anything (audit BP-10): a pointer envelope is ~700 bytes.
const MAX_CANDIDATES = 8, MAX_BYTES = 8192;
const MIN_TRIGGER_MS = 30_000;                     // at most one refresh trigger per repo per 30 s
const lastTrigger = new Map<string, number>();

const APP_ID = "xyz.vpavlin.backpack";
export const topicFor = (id: string) => `/backpack/1/${id}/json`;
// A private repo's notes travel under its read key's id, not the repo key (ADR 0009).
const readKeys = new Map<string, string>();         // repo key → read key (private repos)
/** Tell Delivery a repo's read key (private repos), before watch/loaded need it. */
export function know(key: string, readKey?: string) { if (readKey) readKeys.set(key, readKey); else readKeys.delete(key); }
const topicOf = (key: string) => topicFor(readKeys.has(key) ? topicId(readKeys.get(key)!) : key);
const joined = new Set<string>();                   // topics joined on the node

let state: "off" | "starting" | "on" | "unavailable" = "off";
let lastError = "";
const keys = new Set<string>();
const best = new Map<string, Pointer>();            // repo key → newest valid pointer seen over Delivery
const via = new Map<string, "live" | "store" | "saved">();    // how that pointer arrived
const heard = new Map<string, number>();            // repo key → when a valid note last arrived (any seq)
let listeningSince = 0;
let unavailableAt = 0;                             // a pointer signed before this came from Store, not live
let onPointer: ((p: Pointer) => void) | null = null;

/** Newest pointer for `key` received over Delivery (live or from Store), if any. */
export function pointerFor(key: string): Pointer | undefined { return best.get(key); }
export function arrivedVia(key: string): "live" | "store" | "saved" | undefined { return via.get(key); }
export function status(): { state: typeof state; error: string } { return { state, error: lastError }; }

const SAVED = "backpack.deliveryPointers.v1";      // repo key → the newest note's envelope text
let saved: Record<string, string> = {};
let loading: Promise<void> | null = null;
/** Load the saved notes (once). Each is verified again; a bad or foreign one is dropped. */
export function loaded(): Promise<void> {
  return loading ??= (async () => {
    try { saved = JSON.parse((await AsyncStorage.getItem(SAVED)) ?? "{}") ?? {}; } catch { saved = {}; }
    for (const [key, text] of Object.entries(saved)) {
      try {
        if (typeof text !== "string" || text.length > MAX_BYTES) throw new Error("bad");
        const p = validatePointer(openEnvelope(JSON.parse(text), key, "pointer", readKeys.get(key)), key);
        if (!best.has(key) || p.seq > best.get(key)!.seq) { best.set(key, p); via.set(key, "saved"); }
      } catch { delete saved[key]; }
    }
  })();
}
function save(key: string, text: string | null) {
  if (text === null) delete saved[key]; else saved[key] = text;
  AsyncStorage.setItem(SAVED, JSON.stringify(saved)).catch(() => {});
}

function receive(topic: string, candidates: Uint8Array[]): boolean {
  const key = [...keys].find((k) => topicOf(k) === topic);
  if (!key || !Array.isArray(candidates)) return false;
  for (const c of candidates.slice(0, MAX_CANDIDATES)) {
    if (!c || c.length > MAX_BYTES) continue;
    try {
      const text = new TextDecoder().decode(c);
      const p = validatePointer(openEnvelope(JSON.parse(text), key, "pointer", readKeys.get(key)), key);
      storage.diag(`delivery: pointer ${key.slice(0, 6)} seq ${p.seq}`);
      heard.set(key, Date.now());
      const prev = best.get(key);
      if (!prev || p.seq > prev.seq) {
        best.set(key, p); via.set(key, Date.parse(p.created) < listeningSince ? "store" : "live"); save(key, text);
        const now = Date.now();
        if (now - (lastTrigger.get(key) ?? 0) >= MIN_TRIGGER_MS) { lastTrigger.set(key, now); onPointer?.(p); }
        // a newer pointer inside the window is kept (best) and picked up by the next refresh
      }
      return true;
    } catch { /* not ours, not valid, or another candidate decoding: try the next */ }
  }
  return false;
}

/** Stop caring about removed repos (audit BP-21). The transport has no unsubscribe yet, so the topic
 *  stays joined until restart, but its messages are ignored and nothing is remembered for it. */
export function unwatch(repoKeys: string[]): void {
  for (const k of repoKeys) { readKeys.delete(k); keys.delete(k); best.delete(k); via.delete(k); heard.delete(k); lastTrigger.delete(k); save(k, null); }
}

async function deviceId(): Promise<string> {
  let id = await AsyncStorage.getItem("backpack.deviceId");
  if (!id) { id = `bp-${Math.random().toString(36).slice(2)}${Date.now().toString(36)}`; await AsyncStorage.setItem("backpack.deviceId", id); }
  return id;
}

/** Listen for the given repos' pointers. Safe to call again with more repos (joins the new topics; a repo
 *  that went private moves to its read key's topic). */
export async function watch(repos: { key: string; readKey?: string }[], cb: (p: Pointer) => void): Promise<void> {
  onPointer = cb;
  repos.forEach((r) => know(r.key, r.readKey));
  await loaded();
  repos.forEach((r) => keys.add(r.key));
  const fresh = [...keys].filter((k) => !joined.has(topicOf(k)));
  if (state === "starting") return;                   // joined right after start() (audit R-9)
  if (state === "unavailable") {                     // Loam may have appeared since: retry, at most once a minute
    if (Date.now() - unavailableAt < 60_000) return;
    state = "off";
  }
  try {
    if (state === "on") {
      if (fresh.length) { await T.join(fresh.map(topicOf)); fresh.forEach((k) => joined.add(topicOf(k))); pull(); }
      return;
    }
    state = "starting"; listeningSince = Date.now();
    // must come before anything that wires the transport backend (loam-integrate-app §2)
    T.preferServiceBackend(true, APP_ID);
    const startedWith = new Set(keys);
    await T.start({ deviceId: await deviceId(), topics: [...startedWith].map(topicOf), onReceive: receive, onStatus: (s) => storage.diag(`delivery: ${s}`) });
    startedWith.forEach((k) => joined.add(topicOf(k)));
    state = "on"; lastError = "";
    const late = [...keys].filter((k) => !startedWith.has(k));   // repos added while the node was starting
    if (late.length) { await T.join(late.map(topicOf)); late.forEach((k) => joined.add(topicOf(k))); }
    storage.diag(`delivery: listening on ${keys.size} repo topic(s) via ${T.usingServiceBackend() ? "Loam" : "own node"}`);
    pull();
  } catch (e: any) {
    state = "unavailable"; unavailableAt = Date.now(); lastError = String(e?.message ?? e);
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
