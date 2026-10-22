// Writes test/vectors.json: envelopes made by the CLI's seal(), plus tampered ones, with the expected
// verdict. Both the CLI test (envelope.test.mjs) and the phone verifier test (mobile) read it.
import { generateKeyPairSync, createPublicKey } from "node:crypto";
import fs from "node:fs";
import { seal } from "../envelope.mjs";
const kp = generateKeyPairSync("ed25519"), other = generateKeyPairSync("ed25519");
const key = createPublicKey(kp.privateKey).export({ format: "jwk" }).x;
const okIndex = seal("index", { v: 2, name: "t", key, seq: 3, apps: [] }, kp.privateKey);
const okPtr = seal("pointer", { key, seq: 3, index: "zDvTest" }, kp.privateKey);
const v = [
  { name: "valid index", kind: "index", env: okIndex, ok: true, seq: 3 },
  { name: "valid pointer", kind: "pointer", env: okPtr, ok: true, seq: 3 },
  { name: "body tampered", kind: "index", env: { ...okIndex, body: okIndex.body.replace('"seq":3', '"seq":9') }, ok: false },
  { name: "signed by another key", kind: "index", env: seal("index", { v: 2, name: "t", key, seq: 4, apps: [] }, other.privateKey), ok: false },
  { name: "signed for another repo", kind: "index", env: seal("index", { v: 2, name: "t", key: "X".repeat(43), seq: 4, apps: [] }, kp.privateKey), ok: false },
  { name: "pointer used as index", kind: "index", env: okPtr, ok: false },
  { name: "index used as pointer", kind: "pointer", env: okIndex, ok: false },
  { name: "sig stripped", kind: "index", env: { backpack: 1, body: okIndex.body }, ok: false },
];
fs.writeFileSync(new URL("./vectors.json", import.meta.url), JSON.stringify({ key, vectors: v }, null, 1));
console.log(`wrote ${v.length} vectors`);
