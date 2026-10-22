// The phone's verifier against the CLI's vectors: node --experimental-strip-types test/envelope-vectors.test.ts
import assert from "node:assert";
import fs from "node:fs";
import { openEnvelope } from "../src/envelope.ts";
const { key, vectors } = JSON.parse(fs.readFileSync(new URL("../../cli/test/vectors.json", import.meta.url), "utf8"));
for (const t of vectors) {
  let ok = true, seq: number | undefined;
  try { seq = openEnvelope(t.env, key, t.kind).seq; } catch { ok = false; }
  assert.equal(ok, t.ok, t.name);
  if (t.ok) assert.equal(seq, t.seq, t.name);
}
console.log(`phone verifier: ${vectors.length} vectors ok`);
