// member.ts - member repos on the phone (ADR 0013): this device's X25519 key, its member link (QR),
// and opening a repo's key bundle to get the current epoch's read key.
//   blob = ephemeral pk (32) | nonce (12) | AES-256-GCM(key, nonce, readKey(32), aad) (48)
//   key  = HKDF-SHA256(ikm = X25519(device sk, ephemeral pk), salt = empty, info = "backpack/keywrap/1", 32)
//   aad  = repo key (32) | device pk (32) | epoch (u64 LE)
import AsyncStorage from "@react-native-async-storage/async-storage";
import { x25519 } from "@noble/curves/ed25519.js";
import { hkdf } from "@noble/hashes/hkdf.js";
import { sha256 } from "@noble/hashes/sha2.js";
import { gcm } from "@noble/ciphers/aes.js";

const fromB64u = (s: string): Uint8Array => {
  const b = s.replace(/-/g, "+").replace(/_/g, "/");
  const bin = globalThis.atob(b + "===".slice((b.length + 3) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
};
export const toB64u = (u: Uint8Array): string => {
  let s = ""; for (const c of u) s += String.fromCharCode(c);
  return globalThis.btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
};
const B64U43 = /^[A-Za-z0-9_-]{43}$/;

// ---- this device's key ------------------------------------------------------------------------------
const KEY = "backpack.deviceKey.v1";
let cached: { sk: Uint8Array; pk: Uint8Array } | null = null;
// Hermes has no global crypto.getRandomValues: expo-crypto's comes from the OS (SecureRandom).
// (loaded lazily: the unwrap tests run under plain Node, where the native module doesn't exist)
function randomBytes(n: number): Uint8Array { return (require("expo-crypto") as typeof import("expo-crypto")).getRandomBytes(n); }
export async function deviceKey(): Promise<{ sk: Uint8Array; pk: Uint8Array }> {
  if (cached) return cached;
  let s = await AsyncStorage.getItem(KEY);
  if (!s || !B64U43.test(s)) { s = toB64u(randomBytes(32)); await AsyncStorage.setItem(KEY, s); }
  const sk = fromB64u(s); cached = { sk, pk: x25519.getPublicKey(sk) };
  return cached;
}
/** What the publisher scans to add this phone as a member. Not secret. */
export async function memberLink(deviceName: string): Promise<string> {
  const { pk } = await deviceKey();
  return `backpack://member?pk=${toB64u(pk)}&n=${encodeURIComponent(deviceName.slice(0, 40))}`;
}

// ---- the key bundle ------------------------------------------------------------------------------------
function aad(repoKey: string, devicePk: Uint8Array, epoch: number): Uint8Array {
  const rk = fromB64u(repoKey); if (rk.length !== 32) throw new Error("bad repo key");
  const a = new Uint8Array(72); a.set(rk, 0); a.set(devicePk, 32);
  let e = BigInt(epoch); for (let i = 0; i < 8; i++) { a[64 + i] = Number(e & 255n); e >>= 8n; }
  return a;
}
export function unwrap(blobB64u: string, sk: Uint8Array, pk: Uint8Array, repoKey: string, epoch: number): string {
  const blob = fromB64u(blobB64u);
  if (blob.length !== 92) throw new Error("bad key blob");
  const shared = x25519.getSharedSecret(sk, blob.subarray(0, 32));
  const k = hkdf(sha256, shared, new Uint8Array(0), new TextEncoder().encode("backpack/keywrap/1"), 32);
  try { return toB64u(gcm(k, blob.subarray(32, 44), aad(repoKey, pk, epoch)).decrypt(blob.subarray(44))); }
  catch { throw new Error("this repo's key for you doesn't open (wrong device key or damaged bundle)"); }
}
/** The epoch's read key from a VERIFIED bundle body, or null when this device isn't in it. */
export async function readKeyFromBundle(body: any, repoKey: string, epoch: number): Promise<string | null> {
  if (!body || typeof body !== "object" || body.key !== repoKey || body.epoch !== epoch || !body.wrapped || typeof body.wrapped !== "object") throw new Error("the key bundle doesn't belong to this repo and epoch");
  const { sk, pk } = await deviceKey();
  const blob = body.wrapped[toB64u(pk)];
  return typeof blob === "string" ? unwrap(blob, sk, pk, repoKey, epoch) : null;
}
