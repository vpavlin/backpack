// schema.test.ts — real published indexes must validate; the audit's malicious shapes must not.
// Run: node --experimental-strip-types test/schema.test.ts [index-envelope.json …]
import { readFileSync } from "node:fs";
import { validateIndex, validatePointer, safePointerUrl, selfAddr } from "../src/schema.ts";

let fail = 0;
const ok = (c: boolean, what: string) => { if (!c) { fail++; console.log("FAIL", what); } };
const throws = (f: () => unknown, what: string) => { try { f(); ok(false, `${what} should be refused`); } catch { /* refused */ } };

for (const file of process.argv.slice(2)) {
  const env = JSON.parse(readFileSync(file, "utf8"));
  const body = JSON.parse(env.body);
  try { const ix = validateIndex(body, body.key); console.log(`ok ${file}: ${ix.apps.length} apps, seq ${ix.seq}`); }
  catch (e: any) { ok(false, `${file}: ${e.message}`); }
}

const K = "BmWXKkN9C3Yp7Fzk15I5Ki2Yqd2DYKQi-fePPU0Kvco", CID = "zDvZRwzm34MsUXoPzMZ2NMU79Aue1BHf5Gc7gCerG5pbx2a7uew4";
const ver = { versionCode: 1, versionName: "1", size: 10, sha256: "a".repeat(64), signer: "b".repeat(64), cid: CID, minSdk: 24, abis: ["arm64-v8a"] };
const good = { v: 2, name: "x", key: K, seq: 1, created: "2026-10-23T00:00:00Z", nodes: [], pointers: [], apps: [{ id: "com.example.app", label: "App", icon: null, versions: [ver] }] };
ok(!!validateIndex(good, K), "a good index validates");
throws(() => validateIndex({ ...good, apps: {} }, K), "apps as an object (BP-4)");
throws(() => validateIndex({ ...good, apps: [{ ...good.apps[0], versions: [] }] }, K), "an app without versions (BP-4)");
throws(() => validateIndex({ ...good, apps: [{ ...good.apps[0], label: null }] }, K), "a null label (BP-4)");
throws(() => validateIndex({ ...good, apps: [{ ...good.apps[0], versions: [{ ...ver, signer: undefined }] }] }, K), "a version without a signer (BP-6)");
throws(() => validateIndex({ ...good, apps: [{ ...good.apps[0], id: "../../../shared_prefs/backpack" }] }, K), "a traversal package id (BP-7)");
throws(() => validateIndex({ ...good, apps: [{ ...good.apps[0], versions: [{ ...ver, versionCode: "../.." }] }] }, K), "a string versionCode (BP-7)");
throws(() => validateIndex({ ...good, apps: [{ ...good.apps[0], icon: "../x" }] }, K), "a non-CID icon (BP-7)");
throws(() => validateIndex({ key: K, seq: 2, index: CID, created: "x" }, K), "a pointer body as an index (BP-4)");
throws(() => validateIndex({ ...good, key: "other" }, K), "an index for another repo");
throws(() => validateIndex({ ...good, apps: Array(501).fill(good.apps[0]) }, K), "more than 500 apps (BP-22)");
ok(validateIndex({ ...good, apps: [{ ...good.apps[0], source: "intent://evil" }] }, K).apps[0].source === undefined, "non-http source dropped (BP-18)");
ok(validateIndex({ ...good, pointers: ["http://127.0.0.1/x", "https://192.168.1.1/x", "https://[::1]/x", "https://data.vpavlin.xyz/p.json"] }, K).pointers.length === 1, "private/loopback/http pointer URLs dropped (BP-11)");
ok(!!validatePointer({ key: K, seq: 3, index: CID, created: "x" }, K), "a good pointer validates");
throws(() => validatePointer({ key: K, seq: -1, index: CID, created: "x" }, K), "a negative seq");
throws(() => validatePointer({ key: K, seq: 1, index: "../x", created: "x" }, K), "a non-CID pointer index");
ok(safePointerUrl("https://10.0.0.1/x") === undefined && safePointerUrl("https://example.org/x") !== undefined, "safePointerUrl");
for (const a of ["/dns4/localhost./tcp/1", "/dns4/foo.local./tcp/1", "/dns4/127.0.0.1/tcp/1", "/ip6/0:0:0:0:0:ffff:7f00:1/tcp/1", "/ip4/2130706433/tcp/1", "/ip6/::1/tcp/1", "/ip6/fe80::1/tcp/1"]) ok(selfAddr(a), `${a} refused (R-4)`);
for (const a of ["/ip4/128.140.55.128/tcp/8499", "/ip6/fdb0:9afc::1/tcp/1", "/ip4/192.168.1.5/tcp/1", "/dns4/node-01.do-ams3.logos.test.status.im/tcp/30303"]) ok(!selfAddr(a), `${a} allowed`);
console.log(fail ? `${fail} FAILED` : "schema: all ok");
process.exit(fail ? 1 : 0);
