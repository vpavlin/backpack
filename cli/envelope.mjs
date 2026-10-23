// envelope.mjs — Backpack's signed envelope (ADR 0002): {"backpack":1,["kind":"pointer",]"body":"<json>","sig":"<b64url>"},
// sig = ed25519 over the exact UTF-8 bytes of `body`. Shared by the CLI and the tests; the phone has
// its own TypeScript verifier (mobile/src/repo.ts) checked against test/vectors.json.
import { createPublicKey, sign, verify } from "node:crypto";

export function seal(kind, bodyObj, privateKey) {
  const body = JSON.stringify(bodyObj);
  return { backpack: 1, ...(kind === "index" ? {} : { kind }), body, sig: sign(null, Buffer.from(body), privateKey).toString("base64url") };
}

export function open(env, key, kind) {
  if (env?.backpack !== 1 || typeof env.body !== "string" || typeof env.sig !== "string") throw new Error(`not a Backpack ${kind}`);
  // exact kind (audit BP-4): a pointer says "pointer"; an index carries no kind at all
  if (kind === "pointer" ? env.kind !== "pointer" : env.kind !== undefined) throw new Error(`expected a ${kind}`);
  const pub = createPublicKey({ key: { kty: "OKP", crv: "Ed25519", x: key }, format: "jwk" });
  if (!verify(null, Buffer.from(env.body), pub, Buffer.from(env.sig, "base64url"))) throw new Error(`${kind} signature does not match the repo key`);
  const body = JSON.parse(env.body);
  if (body.key !== key) throw new Error(`${kind} was signed for a different repo`);
  return body;
}
