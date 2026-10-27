// envelope.ts — verify a Backpack signed envelope (ADR 0002) on the phone. Pure (no React Native),
// so test/envelope-vectors.test.ts checks it against the CLI's vectors (cli/test/vectors.json).
import { ed25519 } from "@noble/curves/ed25519.js";
import { decBody } from "./crypt";

const b64urlToBytes = (s: string) => {
  const b = s.replace(/-/g, "+").replace(/_/g, "/");
  const bin = globalThis.atob(b + "===".slice((b.length + 3) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
};
const utf8 = (s: string) => new TextEncoder().encode(s);

/** Open a signed envelope: signature by `key`, right kind, signed for this repo. Throws otherwise.
 *  A private repo's envelope (ADR 0009) has `enc`: the signature covers the encrypted body, which is
 *  decrypted with the repo's read key only after the signature checks. */
export function openEnvelope<T = any>(env: any, key: string, kind: "index" | "pointer" | "keys", readKey?: string): T {
  if (env?.backpack !== 1 || typeof env.body !== "string" || typeof env.sig !== "string") throw new Error(`not a Backpack ${kind}`);
  // exact kind (audit BP-4): a pointer says "pointer"; an index carries no kind at all
  if (kind === "index" ? env.kind !== undefined : env.kind !== kind) throw new Error(`expected a ${kind}`);
  let ok = false;
  try { ok = ed25519.verify(b64urlToBytes(env.sig), utf8(env.body), b64urlToBytes(key)); } catch { ok = false; }
  if (!ok) throw new Error(`${kind} signature doesn't match the repo key`);
  if (env.key !== undefined && env.key !== key) throw new Error(`${kind} was signed for a different repo`);
  if (env.enc !== undefined && env.enc !== "a256gcm") throw new Error(`${kind}: unknown encryption`);
  if (env.enc && !readKey) throw new Error(`this is a private repository: open its link again (the one with the read key)`);
  const body = JSON.parse(env.enc ? decBody(readKey!, env.body) : env.body);
  if (body.key !== key) throw new Error(`${kind} was signed for a different repo`);
  return body as T;
}
