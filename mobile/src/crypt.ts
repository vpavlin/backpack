// crypt.ts — private repos on the phone (ADR 0009): the read key's topic id and encrypted envelope bodies.
// Same formats as cli/crypt.mjs (test/crypt-vectors.test.ts checks them against the CLI's vectors).
// Files (APKs, icons) are decrypted natively by the installer module (BackpackInstaller.decryptFile).
import { gcm } from "@noble/ciphers/aes.js";
import { sha256 } from "@noble/hashes/sha2.js";

export const READ_KEY = /^[A-Za-z0-9_-]{43}$/;     // 32 bytes, base64url
export const isReadKey = (k: unknown): k is string => typeof k === "string" && READ_KEY.test(k);

const fromB64u = (s: string): Uint8Array => {
  const b = s.replace(/-/g, "+").replace(/_/g, "/");
  const bin = globalThis.atob(b + "===".slice((b.length + 3) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
};
const toB64u = (u: Uint8Array): string => {
  let s = ""; for (const c of u) s += String.fromCharCode(c);
  return globalThis.btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
};

const keyBytes = (k: string) => { if (!isReadKey(k)) throw new Error("malformed read key"); return fromB64u(k); };

/** The id a private repo's pointer travels under: Delivery topic /backpack/1/<id>/json, web file <id>.json. */
export function topicId(readKey: string): string {
  const k = keyBytes(readKey), tag = new TextEncoder().encode("backpack/topic/1");
  const m = new Uint8Array(tag.length + k.length); m.set(tag); m.set(k, tag.length);
  return toB64u(sha256(m));
}

/** b64url(nonce(12) ‖ AES-256-GCM ciphertext ‖ tag) → the JSON text. Throws on a wrong key or tampering. */
export function decBody(readKey: string, body: string): string {
  if (!/^[A-Za-z0-9_-]+$/.test(body)) throw new Error("encrypted body is not base64url");
  const raw = fromB64u(body);
  if (raw.length < 28) throw new Error("encrypted body too short");
  try { return new TextDecoder().decode(gcm(keyBytes(readKey), raw.subarray(0, 12)).decrypt(raw.subarray(12))); }
  catch { throw new Error("can't decrypt: wrong read key or damaged data"); }
}
