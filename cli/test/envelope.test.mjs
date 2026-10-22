import assert from "node:assert";
import fs from "node:fs";
import { open } from "../envelope.mjs";
const { key, vectors } = JSON.parse(fs.readFileSync(new URL("./vectors.json", import.meta.url)));
for (const t of vectors) {
  let ok = true, seq;
  try { seq = open(t.env, key, t.kind).seq; } catch { ok = false; }
  assert.equal(ok, t.ok, t.name);
  if (t.ok) assert.equal(seq, t.seq, t.name);
}
console.log(`envelope: ${vectors.length} vectors ok`);
