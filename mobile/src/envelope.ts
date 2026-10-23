// envelope.ts — verify a Backpack signed envelope (ADR 0002) on the phone. Pure (no React Native),
// so test/envelope-vectors.test.ts checks it against the CLI's vectors (cli/test/vectors.json).
import { ed25519 } from "@noble/curves/ed25519.js";

const b64urlToBytes = (s: string) => {
  const b = s.replace(/-/g, "+").replace(/_/g, "/");
  const bin = globalThis.atob(b + "===".slice((b.length + 3) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
};
const utf8 = (s: string) => new TextEncoder().encode(s);

/** Open a signed envelope: signature by `key`, right kind, signed for this repo. Throws otherwise. */
export function openEnvelope<T = any>(env: any, key: string, kind: "index" | "pointer"): T {
  if (env?.backpack !== 1 || typeof env.body !== "string" || typeof env.sig !== "string") throw new Error(`not a Backpack ${kind}`);
  // exact kind (audit BP-4): a pointer says "pointer"; an index carries no kind at all
  if (kind === "pointer" ? env.kind !== "pointer" : env.kind !== undefined) throw new Error(`expected a ${kind}`);
  let ok = false;
  try { ok = ed25519.verify(b64urlToBytes(env.sig), utf8(env.body), b64urlToBytes(key)); } catch { ok = false; }
  if (!ok) throw new Error(`${kind} signature doesn't match the repo key`);
  const body = JSON.parse(env.body);
  if (body.key !== key) throw new Error(`${kind} was signed for a different repo`);
  return body as T;
}
