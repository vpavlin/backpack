// source.test.ts — one source repository per installed app (ADR 0017).
// Run: node --experimental-strip-types test/source.test.ts
import { sourceState, updateSource, mayInstall } from "../src/source.ts";

let fail = 0;
const ok = (c: boolean, what: string) => { if (!c) { fail++; console.log("FAIL", what); } };

const P = "com.example.app", A = "repoA", B = "repoB";

// not chosen: one repo → that one; several → nobody until the user picks
ok(sourceState(P, [A], {}).kind === "only", "one repo lists it: that repo");
ok(updateSource(P, [A], {}) === A, "one repo lists it: updates from it");
ok(sourceState(P, [A, B], {}).kind === "ambiguous", "two repos list it: ambiguous");
ok(updateSource(P, [A, B], {}) === null, "two repos, nothing chosen: no automatic updates");
ok(updateSource(P, [], {}) === null, "no repo lists it: nothing");

// chosen: only that repo, even when another one lists it (with a higher version, same key…)
ok(updateSource(P, [A, B], { [P]: A }) === A, "chosen A: updates from A");
ok(updateSource(P, [B], { [P]: A }) === null, "chosen A, only B lists it: B never takes over");
ok(sourceState(P, [B], { [P]: A }).kind === "gone", "chosen repo no longer lists it: gone");
ok(updateSource(P, [A, B], { "other.pkg": B }) === null, "a choice for another package doesn't count");

// installs
ok(mayInstall(P, B, false, [A, B], { [P]: A }), "not installed: any repo the user taps");
ok(mayInstall(P, A, true, [A, B], { [P]: A }), "installed, from its source: yes");
ok(!mayInstall(P, B, true, [A, B], { [P]: A }), "installed, from another repo: no");
ok(mayInstall(P, B, true, [A, B], { [P]: A }, true), "installed, another repo, user confirmed: yes");
ok(!mayInstall(P, A, true, [A, B], {}), "installed, ambiguous, not chosen: needs a choice");
ok(mayInstall(P, A, true, [A], {}), "installed, only one repo lists it: yes");
ok(!mayInstall(P, B, true, [B], { [P]: A }), "installed, its source is gone: another repo still needs confirming");

if (fail) { console.log(`${fail} failed`); process.exit(1); }
console.log("source: all ok");
