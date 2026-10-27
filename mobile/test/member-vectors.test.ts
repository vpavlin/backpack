// The phone unwraps the C++ publisher's key blob (ADR 0013):
// node --import ./test/ts-resolve.mjs --experimental-strip-types test/member-vectors.test.ts
import assert from "node:assert";
import fs from "node:fs";
import { unwrap } from "../src/member.ts";
const v = JSON.parse(fs.readFileSync(new URL("../../basecamp/backpack_core/test/member-vectors.json", import.meta.url), "utf8"));
const b = (s: string) => Uint8Array.from(Buffer.from(s, "base64url"));
assert.equal(unwrap(v.blob, b(v.deviceSk), b(v.devicePk), v.repoKey, v.epoch), v.readKey, "unwraps the C++ blob");
assert.throws(() => unwrap(v.blob, b(v.deviceSk), b(v.devicePk), v.repoKey, v.epoch + 1), /doesn't open/, "wrong epoch");
console.log("phone member: vectors ok");
