// envelope.mjs — Backpack's signed envelope (ADR 0002): {"backpack":1,["kind":"pointer",]"body":"<json>","sig":"<b64url>"},
// sig = ed25519 over the exact UTF-8 bytes of `body`. Shared by the CLI and the tests; the phone has
// its own TypeScript verifier (mobile/src/repo.ts) checked against test/vectors.json.
import { createPublicKey, sign, verify } from "node:crypto";
import { encBody, decBody } from "./crypt.mjs";

export const ENC = "a256gcm";

// Private repos (ADR 0009): with a read key, `body` is the encrypted JSON (crypt.mjs encBody) and
// `enc` says so. The signature covers the `body` string either way, so it is checked before decrypting.
// An encrypted pointer also carries the repo key in the clear (`key`): relays check the signature
// without the read key.
export function seal(kind, bodyObj, privateKey, readKey) {
  const json = JSON.stringify(bodyObj);
  const body = readKey ? encBody(readKey, json) : json;
  const env = { backpack: 1, ...(kind === "index" ? {} : { kind }), ...(readKey ? { enc: ENC } : {}), body };
  if (readKey && kind === "pointer") env.key = bodyObj.key;
  return { ...env, sig: sign(null, Buffer.from(body), privateKey).toString("base64url") };
}

export function open(env, key, kind, readKey) {
  if (env?.backpack !== 1 || typeof env.body !== "string" || typeof env.sig !== "string") throw new Error(`not a Backpack ${kind}`);
  // exact kind (audit BP-4): a pointer says "pointer"; an index carries no kind at all
  if (kind === "pointer" ? env.kind !== "pointer" : env.kind !== undefined) throw new Error(`expected a ${kind}`);
  const pub = createPublicKey({ key: { kty: "OKP", crv: "Ed25519", x: key }, format: "jwk" });
  if (!verify(null, Buffer.from(env.body), pub, Buffer.from(env.sig, "base64url"))) throw new Error(`${kind} signature does not match the repo key`);
  if (env.enc !== undefined && env.enc !== ENC) throw new Error(`${kind}: unknown encryption ${env.enc}`);
  if (env.key !== undefined && env.key !== key) throw new Error(`${kind} was signed for a different repo`);
  if (env.enc && !readKey) throw new Error(`${kind} is encrypted: this is a private repo and the link has no read key`);
  const body = JSON.parse(env.enc ? decBody(readKey, env.body) : env.body);
  if (body.key !== key) throw new Error(`${kind} was signed for a different repo`);
  return body;
}
