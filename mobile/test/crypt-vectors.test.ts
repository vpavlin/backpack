// The phone's private-repo code against the CLI's vectors (ADR 0009):
// node --import ./test/ts-resolve.mjs --experimental-strip-types test/crypt-vectors.test.ts
import assert from "node:assert";
import fs from "node:fs";
import { openEnvelope } from "../src/envelope.ts";
import { topicId } from "../src/crypt.ts";
const v = JSON.parse(fs.readFileSync(new URL("../../cli/test/crypt-vectors.json", import.meta.url), "utf8"));
assert.equal(topicId(v.readKey), v.topicId, "topic id");
assert.equal(openEnvelope(v.env, v.key, "index", v.readKey).seq, 7, "decrypts with the read key");
assert.throws(() => openEnvelope(v.env, v.key, "index"), /private repository/, "refuses without the read key");
assert.throws(() => openEnvelope(v.env, v.key, "index", v.otherKey), /can't decrypt/, "refuses a wrong read key");
assert.throws(() => openEnvelope({ ...v.env, body: v.env.body.slice(0, -2) + "AA" }, v.key, "index", v.readKey), /signature/, "a changed body fails the signature first");
console.log("phone crypt: vectors ok");
