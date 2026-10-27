// Private repos (ADR 0009): crypt.mjs round trips and failure cases, and the envelope with a read key.
// make-vectors (below, with --write) records fixed cases the phone's TypeScript must also accept/refuse
// (mobile/test/crypt-vectors.test.ts).
import assert from "node:assert";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { generateKeyPairSync } from "node:crypto";
import { newKey, topicId, encBody, decBody, encryptFile, decryptBuffer, CHUNK } from "../crypt.mjs";
import { seal, open } from "../envelope.mjs";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "bp-crypt-"));
const R = newKey();

// body
assert.equal(decBody(R, encBody(R, "héllo")), "héllo");
assert.throws(() => decBody(newKey(), encBody(R, "x")), /wrong read key/);

// files: empty, small, exactly one chunk, several chunks
for (const n of [0, 5, CHUNK, CHUNK * 2 + 7]) {
  const src = path.join(tmp, `p${n}`), dst = src + ".bpe";
  const data = Buffer.alloc(n); for (let i = 0; i < n; i++) data[i] = (i * 31) & 255;
  fs.writeFileSync(src, data);
  const { key, size } = encryptFile(src, dst);
  const ct = fs.readFileSync(dst);
  assert.equal(ct.length, size);
  assert.deepEqual(decryptBuffer(ct, key), data, `round trip ${n}`);
  assert.throws(() => decryptBuffer(ct, newKey()), /can't decrypt/);
  if (n > CHUNK) {
    assert.throws(() => decryptBuffer(ct.subarray(0, 16 + CHUNK + 16), key), /can't decrypt|truncated/, "cut at a chunk boundary");
    const flip = Buffer.from(ct); flip[20] ^= 1; assert.throws(() => decryptBuffer(flip, key), /can't decrypt/);
  }
}

// envelopes with a read key
const { privateKey, publicKey } = generateKeyPairSync("ed25519");
const K = publicKey.export({ format: "jwk" }).x;
const idx = seal("index", { key: K, seq: 3, apps: [] }, privateKey, R);
assert.equal(idx.enc, "a256gcm");
assert.equal(open(idx, K, "index", R).seq, 3);
assert.throws(() => open(idx, K, "index"), /no read key/);
assert.throws(() => open(idx, K, "index", newKey()), /wrong read key/);
const ptr = seal("pointer", { key: K, seq: 3, index: "zX" }, privateKey, R);
assert.equal(ptr.key, K, "an encrypted pointer names its repo key for relays");
assert.equal(open(ptr, K, "pointer", R).index, "zX");
assert.throws(() => open({ ...ptr, body: idx.body }, K, "pointer", R), /signature/);
assert.equal(topicId(R).length, 43);
console.log("crypt: ok");

if (process.argv.includes("--write")) {
  const readKey = newKey(), other = newKey();
  const env = seal("index", { key: K, seq: 7, apps: [] }, privateKey, readKey);
  const src = path.join(tmp, "v"); fs.writeFileSync(src, Buffer.from("x".repeat(CHUNK + 100)));
  const f = encryptFile(src, src + ".bpe");
  fs.writeFileSync(new URL("./crypt-vectors.json", import.meta.url), JSON.stringify({
    key: K, readKey, otherKey: other, topicId: topicId(readKey),
    env, file: { key: f.key, sha256: (await import("node:crypto")).createHash("sha256").update(fs.readFileSync(src)).digest("hex"), b64: fs.readFileSync(src + ".bpe").toString("base64") },
  }, null, 1));
  console.log("wrote test/crypt-vectors.json");
}
