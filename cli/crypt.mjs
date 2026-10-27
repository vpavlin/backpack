// crypt.mjs — private repos (ADR 0009): the read key, encrypted envelope bodies, chunked file encryption.
//
//   envelope body:  b64url(nonce(12) ‖ AES-256-GCM(readKey, nonce, utf8 json) ‖ tag(16))
//   file (bpe1):    "BPE1" ‖ chunk size u32 BE ‖ nonce prefix(8) ‖ chunk*
//                   chunk = AES-256-GCM(fileKey, prefix ‖ u32 BE index, aad = [last ? 1 : 0]) ‖ tag(16)
//   topic id:       b64url(SHA-256("backpack/topic/1" ‖ readKey)) — names the Delivery topic and web file
// The phone has the same in mobile/src/crypt.ts (index, pointer, topic) and the native installer (files);
// test/crypt.test.mjs pins them with vectors.
import { createCipheriv, createDecipheriv, createHash, randomBytes } from "node:crypto";
import fs from "node:fs";

export const CHUNK = 1 << 20;
const MAGIC = Buffer.from("BPE1");
const TAG = 16;

const b64u = (b) => Buffer.from(b).toString("base64url");
const unb64u = (s, n) => {
  if (typeof s !== "string" || !/^[A-Za-z0-9_-]+$/.test(s)) throw new Error("not base64url");
  const b = Buffer.from(s, "base64url");
  if (n !== undefined && b.length !== n) throw new Error(`expected ${n} bytes, got ${b.length}`);
  return b;
};

export const newKey = () => b64u(randomBytes(32));
export const keyBytes = (k) => unb64u(k, 32);

/** The id a private repo's pointer travels under: Delivery topic /backpack/1/<id>/json, web file <id>.json. */
export const topicId = (readKey) => b64u(createHash("sha256").update("backpack/topic/1").update(keyBytes(readKey)).digest());

export function encBody(readKey, text) {
  const nonce = randomBytes(12);
  const c = createCipheriv("aes-256-gcm", keyBytes(readKey), nonce);
  const ct = Buffer.concat([c.update(Buffer.from(text, "utf8")), c.final()]);
  return b64u(Buffer.concat([nonce, ct, c.getAuthTag()]));
}

export function decBody(readKey, body) {
  const raw = unb64u(body);
  if (raw.length < 12 + TAG) throw new Error("encrypted body too short");
  const d = createDecipheriv("aes-256-gcm", keyBytes(readKey), raw.subarray(0, 12));
  d.setAuthTag(raw.subarray(raw.length - TAG));
  try { return Buffer.concat([d.update(raw.subarray(12, raw.length - TAG)), d.final()]).toString("utf8"); }
  catch { throw new Error("can't decrypt: wrong read key or damaged data"); }
}

const chunkNonce = (prefix, i) => { const n = Buffer.alloc(12); prefix.copy(n, 0); n.writeUInt32BE(i, 8); return n; };

/** Encrypt src → dst (bpe1) with a fresh file key. Returns { key, size } (size = ciphertext bytes). */
export function encryptFile(src, dst) {
  const key = randomBytes(32), prefix = randomBytes(8);
  const fin = fs.openSync(src, "r"), fout = fs.openSync(dst, "w");
  try {
    const head = Buffer.alloc(16); MAGIC.copy(head, 0); head.writeUInt32BE(CHUNK, 4); prefix.copy(head, 8);
    fs.writeSync(fout, head);
    const total = fs.fstatSync(fin).size;
    const n = Math.max(1, Math.ceil(total / CHUNK));   // an empty file is one empty, last chunk
    const buf = Buffer.alloc(CHUNK);
    let size = head.length;
    for (let i = 0; i < n; i++) {
      const got = fs.readSync(fin, buf, 0, CHUNK, i * CHUNK);
      const c = createCipheriv("aes-256-gcm", key, chunkNonce(prefix, i));
      c.setAAD(Buffer.from([i === n - 1 ? 1 : 0]));
      const ct = Buffer.concat([c.update(buf.subarray(0, got)), c.final(), c.getAuthTag()]);
      fs.writeSync(fout, ct); size += ct.length;
    }
    return { key: b64u(key), size };
  } finally { fs.closeSync(fin); fs.closeSync(fout); }
}

/** Decrypt a bpe1 file (Buffer) with its file key. Throws on any tampering, truncation or reordering. */
export function decryptBuffer(data, fileKey) {
  const key = keyBytes(fileKey);
  if (data.length < 16 || !data.subarray(0, 4).equals(MAGIC)) throw new Error("not a Backpack encrypted file");
  const chunk = data.readUInt32BE(4), prefix = data.subarray(8, 16);
  if (chunk !== CHUNK) throw new Error(`unexpected chunk size ${chunk}`);   // the header isn't authenticated (audit PR-5)
  const out = [];
  let off = 16, i = 0, last = false;
  while (off < data.length) {
    const len = Math.min(chunk + TAG, data.length - off);
    if (len < TAG) throw new Error("truncated chunk");
    last = off + len === data.length;
    const d = createDecipheriv("aes-256-gcm", key, chunkNonce(prefix, i));
    d.setAAD(Buffer.from([last ? 1 : 0]));
    d.setAuthTag(data.subarray(off + len - TAG, off + len));
    try { out.push(d.update(data.subarray(off, off + len - TAG)), d.final()); }
    catch { throw new Error(`chunk ${i}: can't decrypt (wrong key, damaged or truncated)`); }
    off += len; i++;
  }
  if (!last) throw new Error("truncated: no last chunk");
  return Buffer.concat(out);
}
