#!/usr/bin/env node
// backpack — publish Android apps to a repository that lives in Logos Storage.
// Design and security model: docs/adr/ (0001 repo = key, 0002 security, 0003 updates, 0004 F-Droid import).
//
//   backpack init --name "My backpack" [--node http://[…]:8480]
//   backpack config [--node URL] [--pointer-get URL]… [--pointer-put rsync:host:/path|put:URL|exec:CMD]… [--clear-pointers]
//   backpack add <file.apk>… [--name N --summary S --description D --license L]
//   backpack import-fdroid <repo URL> --fingerprint <sha256> [--versions 1] [--only pkg,pkg]
//   backpack publish                  sign + upload the index, sign + push the pointer, print the link
//   backpack verify [link] [--deep]   check pointer(s), index and (--deep) every latest APK as a phone would
//   backpack ls
//
// State: ~/.backpack/<repo>/repo.json (+ key.pem, 0600). The node is a Logos Storage node's REST API
// (no auth: use one reachable only by you — the mesh or localhost).
import { randomUUID, createHash, createPrivateKey, createPublicKey, generateKeyPairSync } from "node:crypto";
import { seal, open } from "./envelope.mjs";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const DEFAULT_NODE = "http://[fdb0:9afc:a5ef:f167:7ad7:68b7:aca0:b9fa]:8480"; // VPS storage node, mesh-only REST
const BT = process.env.BUILD_TOOLS ?? latestBuildTools();
const AAPT = process.env.AAPT ?? path.join(BT, "aapt2");
const APKSIGNER = process.env.APKSIGNER ?? path.join(BT, "apksigner");
const KEEP_VERSIONS = 3;

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(`--${name}`); if (i < 0) return def; const v = args[i + 1]; args.splice(i, 2); return v; };
const optAll = (name) => { const out = []; for (let v; (v = opt(name, null)) !== null;) out.push(v); return out; };
const flag = (name) => { const i = args.indexOf(`--${name}`); if (i < 0) return false; args.splice(i, 1); return true; };
const repoName = opt("repo", "default");
const dir = path.join(os.homedir(), ".backpack", repoName);
const stateFile = path.join(dir, "repo.json"), keyFile = path.join(dir, "key.pem");
const die = (m) => { console.error(`backpack: ${m}`); process.exit(1); };
const tmpDir = () => fs.mkdtempSync(path.join(os.tmpdir(), "backpack-"));

function latestBuildTools() {
  const bt = path.join(os.homedir(), "Android/Sdk/build-tools");
  try {
    const v = fs.readdirSync(bt).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).reverse()[0];
    return path.join(bt, v);
  } catch { return ""; }
}
function load() { if (!fs.existsSync(stateFile)) die(`no repo "${repoName}" — run: backpack init --name "…"`); return JSON.parse(fs.readFileSync(stateFile, "utf8")); }
function save(s) { fs.writeFileSync(stateFile, JSON.stringify(s, null, 2)); }
function privKey() { return createPrivateKey(fs.readFileSync(keyFile)); }
function pubOf() { return createPublicKey(privKey()).export({ format: "jwk" }).x; }
const sha256hex = (buf) => createHash("sha256").update(buf).digest("hex");

// ── signed envelopes (ADR 0002) ──────────────────────────────────────────────
const envelope = (kind, bodyObj) => seal(kind, bodyObj, privKey());
const openEnvelope = open;

// ── Storage node REST ────────────────────────────────────────────────────────
// Storage's block exchange pays roughly a round trip per block, so a far node delivers ~60-120 KB/s with
// the default 64 KiB blocks; 512 KiB blocks (the maximum) measured 3.4x faster (ADR 0006). Small files
// (the index, icons) keep the default.
const BIG_BLOCK = 524288;
const blockSizeFor = (n) => (n > 1 << 17 ? BIG_BLOCK : 65536);   // > 128 KiB: every APK
async function upload(node, file, name, mime) {
  const body = fs.readFileSync(file);
  const q = blockSizeFor(body.length) === BIG_BLOCK ? `?blockSize=${BIG_BLOCK}` : "";
  const r = await fetch(`${node}/api/storage/v1/data${q}`, {
    method: "POST", body,
    headers: { "Content-Type": mime, "Content-Disposition": `attachment; filename="${name}"` },
  });
  if (!r.ok) die(`upload ${name}: HTTP ${r.status} ${await r.text()}`);
  return (await r.text()).trim();
}
async function download(node, cid) {
  for (const p of [`/api/storage/v1/data/${cid}`, `/api/storage/v1/data/${cid}/network/stream`]) {
    const r = await fetch(`${node}${p}`);
    if (r.ok) return Buffer.from(await r.arrayBuffer());
  }
  throw new Error(`node can't serve ${cid}`);
}
async function nodeInfo(node) {
  const r = await fetch(`${node}/api/storage/v1/debug/info`);
  if (!r.ok) die(`node ${node}: HTTP ${r.status}`);
  const j = await r.json();
  return { peerId: j.id, addrs: j.addrs, spr: j.spr };
}

// ── APK inspection ───────────────────────────────────────────────────────────
function badging(apk) {
  const out = execFileSync(AAPT, ["dump", "badging", apk], { encoding: "utf8", maxBuffer: 64 << 20 });
  const get = (re) => out.match(re)?.[1];
  const icons = [...out.matchAll(/application-icon-(\d+):'([^']+)'/g)].map((m) => [Number(m[1]), m[2]]).sort((a, b) => b[0] - a[0]);
  return {
    id: get(/package: name='([^']+)'/),
    versionCode: Number(get(/versionCode='(\d+)'/)),
    versionName: get(/versionName='([^']*)'/) ?? "",
    label: get(/application-label:'([^']*)'/) ?? get(/package: name='([^']+)'/),
    minSdk: Number(get(/minSdkVersion:'(\d+)'/) ?? 0),
    abis: (get(/native-code: (.+)/) ?? "").replace(/'/g, "").split(" ").filter(Boolean),
    icon: rasterIcon(apk, icons[0]?.[1] ?? get(/application: .*icon='([^']+)'/) ?? null),
  };
}
// The badging icon is often an adaptive-icon XML; its resource also has per-density rasters. Take the largest.
function rasterIcon(apk, icon) {
  if (!icon || /\.(png|webp)$/.test(icon)) return icon;
  const dump = execFileSync(AAPT, ["dump", "resources", apk], { encoding: "utf8", maxBuffer: 256 << 20 });
  const block = dump.split(/\n(?=\s*resource 0x)/).find((b) => b.includes(`(file) ${icon}`));
  if (!block) return null;
  const order = ["xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", ""];
  const files = [...block.matchAll(/\((\w*)[^)]*\) \(file\) (\S+\.(?:png|webp))/g)].map((m) => [m[1], m[2]]);
  files.sort((a, b) => order.indexOf(a[0]) - order.indexOf(b[0]));
  return files[0]?.[1] ?? null;
}
// SHA-256 of the APK's (first) signing certificate. Refuses an APK that doesn't verify. (ADR 0002)
function apkSigner(apk) {
  let out;
  try { out = execFileSync(APKSIGNER, ["verify", "--print-certs", apk], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }); }
  catch (e) { throw new Error(`${path.basename(apk)} is not validly signed: ${String(e.stderr || e.message).split("\n")[0]}`); }
  const m = out.match(/Signer #1 certificate SHA-256 digest: ([0-9a-f]{64})/);
  if (!m) throw new Error(`${path.basename(apk)}: no signing certificate found`);
  return m[1];
}

// Upload one APK (+ its icon unless one is given) and record the version. Idempotent per (pkg, versionCode, sha256).
async function addApk(s, apk, meta = {}) {
  const m = badging(apk);
  if (!m.id || !m.versionCode) throw new Error(`${apk}: not an APK aapt2 can read`);
  const bytes = fs.readFileSync(apk);
  const sha256 = sha256hex(bytes);
  if (meta.expectSha256 && meta.expectSha256 !== sha256) throw new Error(`${m.id} ${m.versionCode}: SHA-256 differs from the source index`);
  if (meta.expectId && meta.expectId !== m.id) throw new Error(`the APK is ${m.id}, but the source index says ${meta.expectId}`);
  if (meta.expectVersionCode !== undefined && meta.expectVersionCode !== m.versionCode) throw new Error(`${m.id}: the APK is versionCode ${m.versionCode}, the source index says ${meta.expectVersionCode}`);
  if (!PKG_RE.test(m.id)) throw new Error(`${m.id}: not a valid package name`);
  const signer = apkSigner(apk);
  if (meta.expectSigner && meta.expectSigner !== signer) throw new Error(`${m.id} ${m.versionCode}: signer ${signer.slice(0, 8)}… differs from the source index ${meta.expectSigner.slice(0, 8)}…`);
  const app = (s.apps[m.id] ??= { id: m.id, label: m.label, icon: null, versions: [] });
  const known = app.versions.find((v) => v.versionCode === m.versionCode);
  if (known && known.sha256 === sha256 && known.signer) { console.log(`${m.id} ${m.versionName} (${m.versionCode}) already in the repo`); return false; }
  // the app's signer must not change between versions: Android would refuse the update anyway
  const prev = app.versions.find((v) => v.signer && v.versionCode !== m.versionCode);
  if (prev && prev.signer !== signer) throw new Error(`${m.id}: ${m.versionCode} is signed by ${signer.slice(0, 8)}…, earlier versions by ${prev.signer.slice(0, 8)}…; refusing`);
  process.stdout.write(`${m.id} ${m.versionName} (${m.versionCode}) ${(bytes.length / 1e6).toFixed(1)} MB → uploading… `);
  const cid = await upload(s.node, apk, `${m.id}_${m.versionCode}.apk`, "application/vnd.android.package-archive");
  let iconCid = app.icon;
  if (meta.iconFile) iconCid = await upload(s.node, meta.iconFile, path.basename(meta.iconFile), meta.iconFile.endsWith(".webp") ? "image/webp" : "image/png");
  else if (m.icon && !iconCid) {
    const ext = m.icon.endsWith(".webp") ? "webp" : "png";
    const t = path.join(tmpDir(), `icon.${ext}`);
    fs.writeFileSync(t, execFileSync("unzip", ["-p", apk, m.icon], { maxBuffer: 16 << 20 }));
    iconCid = await upload(s.node, t, `${m.id}.${ext}`, `image/${ext}`);
  }
  app.label = meta.name || app.label || m.label;
  for (const k of ["summary", "description", "license", "source"]) if (meta[k]) app[k] = meta[k];
  app.icon = iconCid;
  app.versions = [{ versionCode: m.versionCode, versionName: m.versionName, size: bytes.length, sha256, signer, cid, blockSize: blockSizeFor(bytes.length), minSdk: m.minSdk, abis: m.abis, added: new Date().toISOString() },
    ...app.versions.filter((v) => v.versionCode !== m.versionCode)].sort((a, b) => b.versionCode - a.versionCode).slice(0, KEEP_VERSIONS);
  console.log(cid);
  return true;
}

// ── commands ─────────────────────────────────────────────────────────────────
async function init() {
  const name = opt("name", null) ?? die("--name is required");
  const node = opt("node", DEFAULT_NODE);
  if (fs.existsSync(stateFile)) die(`repo "${repoName}" already exists (${dir})`);
  fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
  const { privateKey } = generateKeyPairSync("ed25519");
  fs.writeFileSync(keyFile, privateKey.export({ format: "pem", type: "pkcs8" }), { mode: 0o600 });
  save({ name, node, seq: 0, pointers: { get: [], put: [] }, apps: {} });
  console.log(`repo "${name}" created in ${dir}\nkey ${pubOf()}\nnode ${node}\nback up ${keyFile}: without it this repo can never publish again`);
}

async function config() {
  const s = load();
  s.pointers ??= { get: [], put: [] };
  if (flag("clear-pointers")) s.pointers = { get: [], put: [] };
  const node = opt("node", null); if (node) s.node = node;
  for (const g of optAll("pointer-get")) if (!s.pointers.get.includes(g)) s.pointers.get.push(g);
  for (const p of optAll("pointer-put")) {
    if (!/^(rsync|put|exec):/.test(p)) die(`--pointer-put must be rsync:<dest>, put:<url> or exec:<command>, got ${p}`);
    if (!s.pointers.put.includes(p)) s.pointers.put.push(p);
  }
  save(s);
  console.log(JSON.stringify({ node: s.node, seq: s.seq ?? 0, pointers: s.pointers }, null, 2));
}

async function add() {
  const s = load();
  const meta = { name: opt("name", null), summary: opt("summary", null), description: opt("description", null), license: opt("license", null) };
  if (!args.length) die("add <file.apk>…");
  for (const apk of [...args]) {
    try { await addApk(s, apk, meta); } catch (e) { die(e.message); }
  }
  save(s);
  console.log("recorded; run `backpack publish` to make it visible");
}

// F-Droid repo → this repo, only through a verified F-Droid **v2** index (ADR 0004, amended after the
// 2026-10-23 audit). entry.jar is signed with SHA-256 and verifies with the JDK's stock settings (the v1
// index-v1.jar needed SHA-1, and the old workaround re-enabled every disabled JAR algorithm, MD5 included:
// audit BP-3). entry.json pins index-v2.json by SHA-256; index-v2 pins every icon and APK by SHA-256.
// Nothing from the index is ever used as a local path (audit BP-1): files go to generated names.
const SAFE_REMOTE = /^\/?[A-Za-z0-9][A-Za-z0-9._-]*(\/[A-Za-z0-9][A-Za-z0-9._-]*)*$/;   // no "..", no leading dots, no backslashes
const PKG_RE = /^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$/;
// Streams with a hard byte cap: never holds more than maxBytes, whatever the server sends (audit R-7).
async function fetchChecked(url, sha, maxBytes) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const declared = Number(r.headers.get("content-length") ?? "0");
  if (maxBytes && declared > maxBytes) throw new Error(`${declared} bytes, over the ${maxBytes} limit`);
  const parts = []; let n = 0;
  for await (const chunk of r.body) {
    n += chunk.length;
    if (maxBytes && n > maxBytes) { try { await r.body.cancel(); } catch { /* */ } throw new Error(`over the ${maxBytes}-byte limit`); }
    parts.push(chunk);
  }
  const buf = Buffer.concat(parts.map((c) => Buffer.from(c)));
  if (sha && sha256hex(buf) !== sha) throw new Error("SHA-256 doesn't match the signed index");
  return buf;
}
const MAX_APPS = 500;   // what a phone accepts in one repo (mobile/src/schema.ts LIMITS.apps)
async function importFdroid() {
  const s = load();
  const fingerprint = (opt("fingerprint", null) ?? die("--fingerprint <sha256 of the F-Droid repo cert> is required")).replace(/[: ]/g, "").toLowerCase();
  const versions = Number(opt("versions", "1"));
  const only = (opt("only", "") || "").split(",").filter(Boolean);
  const repoUrl = (args[0] ?? die("import-fdroid <repo URL> --fingerprint …")).replace(/\/+$/, "");
  const t = tmpDir();
  const jar = path.join(t, "entry.jar");
  try { fs.writeFileSync(jar, await fetchChecked(`${repoUrl}/entry.jar`, null, 1 << 20)); }
  catch (e) { die(`${repoUrl}/entry.jar: ${e.message} (Backpack imports only F-Droid v2 repos)`); }
  // 1. entry.jar must verify with the JDK's DEFAULT algorithm policy, signed by the cert the user vouched for
  let ver;
  try { ver = execFileSync("jarsigner", ["-verify", jar], { encoding: "utf8" }); } catch (e) { die(`entry.jar does not verify: ${e.stdout || e.message}`); }
  if (!/jar verified/.test(ver)) die(`entry.jar does not verify:\n${ver}`);
  // SHA-1 is collision-prone; JDKs still accept a SHA1withRSA *signature* by default, and f-droid.org signs
  // its entry.jar that way (audit BP-3 partial). Refuse it unless the user explicitly accepts the risk.
  const algs = execFileSync("jarsigner", ["-verify", "-verbose", "-certs", jar], { encoding: "utf8" });
  if (/SHA1withRSA|SHA-1|SHA1-Digest/i.test(algs) && !flag("allow-sha1")) {
    die("entry.jar is signed or digested with SHA-1. SHA-1 collisions are practical, so a hostile mirror could forge it.\n" +
        "If you trust the source and the path to it, re-run with --allow-sha1 (f-droid.org itself needs this today).");
  }
  const certs = execFileSync("keytool", ["-printcert", "-jarfile", jar], { encoding: "utf8" });
  const fps = [...certs.matchAll(/SHA256: ([0-9A-F:]+)/g)].map((m) => m[1].replace(/:/g, "").toLowerCase());
  if (fps.length !== 1 || fps[0] !== fingerprint) die(`entry.jar is signed by ${fps.join(", ") || "nobody"}, not ${fingerprint}`);
  const entry = JSON.parse(execFileSync("unzip", ["-p", jar, "entry.json"], { maxBuffer: 1 << 20, encoding: "utf8" }));
  // freshness (audit R-6): never go back to an older entry from the same source, and refuse one older than
  // its own maxAge (days) unless --allow-stale: a mirror replaying an old signed entry can't hide updates
  const ts = Number(entry.timestamp);
  if (!Number.isFinite(ts)) die("entry.json has no timestamp");
  s.fdroidSeen ??= {};
  if (s.fdroidSeen[fingerprint] && ts < s.fdroidSeen[fingerprint]) die(`entry.json is older (${new Date(ts).toISOString()}) than one already seen from this repo (${new Date(s.fdroidSeen[fingerprint]).toISOString()}): refusing a rollback`);
  const maxAgeDays = Number(entry.maxAge ?? 0);
  if (maxAgeDays > 0 && Date.now() - ts > maxAgeDays * 86400_000 && !flag("allow-stale")) die(`entry.json is ${Math.round((Date.now() - ts) / 86400_000)} days old, over its maxAge of ${maxAgeDays}: refusing (--allow-stale to accept)`);
  // 2. index-v2.json, pinned by entry.json
  const ix = entry.index ?? die("entry.json has no index");
  if (!SAFE_REMOTE.test(ix.name ?? "") || !/^[0-9a-f]{64}$/.test(ix.sha256 ?? "")) die("entry.json: bad index name or hash");
  let index;
  const ixMax = 256 << 20;   // f-droid.org's index-v2 is ~67 MB (audit R-2)
  if (!(Number(ix.size) > 0 && Number(ix.size) <= ixMax)) die(`index-v2.json is ${ix.size} bytes; the limit is ${ixMax}`);
  try { index = JSON.parse((await fetchChecked(`${repoUrl}/${ix.name.replace(/^\//, "")}`, ix.sha256, Number(ix.size))).toString("utf8")); }
  catch (e) { die(`index-v2.json: ${e.message}`); }
  const pkgsAll = Object.entries(index.packages ?? {}).filter(([, p]) => p && typeof p === "object");   // R-7
  const wanted = only.length ? pkgsAll.filter(([k]) => only.includes(k)).length : pkgsAll.length;
  if (wanted + Object.keys(s.apps).length > MAX_APPS) die(`importing ${wanted} apps would take this repo over ${MAX_APPS}, more than a phone accepts; pick some with --only pkg,pkg (audit R-2)`);
  const repoName = index.repo?.name?.["en-US"] ?? index.repo?.name;
  console.log(`verified F-Droid v2 index "${typeof repoName === "string" ? repoName : "?"}" (${fingerprint.slice(0, 16)}…), ${pkgsAll.length} apps`);
  const loc = (o) => (o && typeof o === "object" ? (o["en-US"] ?? o.en ?? Object.values(o)[0]) : o);
  let added = 0;
  for (const [pkg, p] of pkgsAll) {
    if (only.length && !only.includes(pkg)) continue;
    if (!PKG_RE.test(pkg)) { console.log(`${String(pkg).slice(0, 60)}: skipped (not a package name)`); continue; }
    const md = p.metadata ?? {};
    const str = (v, max) => (typeof v === "string" ? v.slice(0, max) : undefined);
    const meta = {
      name: str(loc(md.name), 100), summary: str(loc(md.summary), 300), description: str(loc(md.description), 8000),
      license: str(md.license, 100), source: [md.sourceCode, md.webSite].find((u) => typeof u === "string" && /^https?:\/\//.test(u)),
    };
    // icon: pinned by SHA-256 in the index, written to a generated name
    const icon = loc(md.icon);
    if (icon && SAFE_REMOTE.test(icon.name ?? "") && /^[0-9a-f]{64}$/.test(icon.sha256 ?? "")) {
      try {
        const buf = await fetchChecked(`${repoUrl}/${icon.name.replace(/^\//, "")}`, icon.sha256, 1 << 20);
        meta.iconFile = path.join(t, `icon-${randomUUID()}${icon.name.endsWith(".webp") ? ".webp" : ".png"}`);
        fs.writeFileSync(meta.iconFile, buf);
      } catch (e) { console.log(`${pkg}: icon skipped (${e.message})`); }
    }
    const vs = Object.values(p.versions ?? {}).filter((v) => v?.manifest && Number.isInteger(v.manifest.versionCode))
      .sort((x, y) => y.manifest.versionCode - x.manifest.versionCode).slice(0, versions);
    for (const v of vs.reverse()) {
      const vc = v.manifest.versionCode, f = v.file ?? {};
      const signer = v.manifest.signer?.sha256?.[0];
      if (!SAFE_REMOTE.test(f.name ?? "") || !/^[0-9a-f]{64}$/.test(f.sha256 ?? "") || !/^[0-9a-f]{64}$/.test(signer ?? "")) {
        console.log(`${pkg} ${vc}: skipped (bad file name, hash or signer in the index)`); continue;
      }
      const have = s.apps[pkg]?.versions.find((x) => x.versionCode === vc && x.sha256 === f.sha256 && x.signer);
      if (have) { console.log(`${pkg} ${v.manifest.versionName} (${vc}) already in the repo`); continue; }
      const apk = path.join(t, `apk-${randomUUID()}.apk`);   // generated: never a path from the index
      try {
        fs.writeFileSync(apk, await fetchChecked(`${repoUrl}/${f.name.replace(/^\//, "")}`, f.sha256, 512 << 20));
        if (await addApk(s, apk, { ...meta, expectSha256: f.sha256, expectSigner: signer, expectId: pkg, expectVersionCode: vc })) added++;
      } catch (e) { console.log(`${pkg} ${vc}: NOT imported — ${e.message}`); }
      fs.rmSync(apk, { force: true });
      save(s);
    }
    if (meta.iconFile) fs.rmSync(meta.iconFile, { force: true });
  }
  s.fdroidSeen[fingerprint] = ts;
  save(s);
  console.log(`${added} version(s) imported; run \`backpack publish\` to make them visible`);
}

async function publish() {
  const s = load();
  s.pointers ??= { get: [], put: [] };
  const node = await nodeInfo(s.node);
  const nodesNow = [{ peerId: node.peerId, addrs: node.addrs, spr: node.spr }];
  const apps = Object.values(s.apps).sort((a, b) => a.label.localeCompare(b.label));
  // nothing changed since the last publish → no new seq (phones would just re-download the same list).
  // The SPR is left out: the node re-signs it with a new sequence number now and then.
  const contentHash = sha256hex(Buffer.from(JSON.stringify({ name: s.name, nodes: nodesNow.map((n) => ({ peerId: n.peerId, addrs: [...n.addrs].sort() })), pointers: s.pointers.get, apps })));
  if (contentHash === s.lastContentHash && !flag("force")) {
    console.log(`nothing changed since seq ${s.seq}; not publishing (--force to publish anyway)`);
    return;
  }
  const seq = (s.seq ?? 0) + 1;
  const created = new Date().toISOString();
  const nodes = nodesNow;
  if (apps.length > MAX_APPS) die(`${apps.length} apps: phones accept at most ${MAX_APPS} per repo`);
  const index = envelope("index", { v: 2, name: s.name, key: pubOf(), seq, created, nodes, pointers: s.pointers.get, apps });
  if (JSON.stringify(index).length > 1 << 20) die("the signed index is over 1 MB, more than a phone accepts: trim descriptions or apps");
  const t = tmpDir();
  const indexFile = path.join(t, "backpack-index.json");
  fs.writeFileSync(indexFile, JSON.stringify(index));
  const cid = await upload(s.node, indexFile, "backpack-index.json", "application/json");
  // pointer (ADR 0003): signed like the index, may live anywhere
  const pointer = envelope("pointer", { key: pubOf(), seq, index: cid, created, nodes });
  const pointerFile = path.join(dir, "latest.json");
  fs.writeFileSync(pointerFile, JSON.stringify(pointer));
  s.seq = seq; s.lastIndex = cid; s.lastContentHash = contentHash;
  for (const target of s.pointers.put) {
    try {
      if (target.startsWith("rsync:")) execFileSync("rsync", ["-q", "--chmod=F644", pointerFile, target.slice(6)], { stdio: "inherit" });
      // exec: any command, e.g. announcing over Logos Delivery (ADR 0007); gets the key and the file
      else if (target.startsWith("exec:")) execFileSync("sh", ["-c", target.slice(5)], { stdio: "inherit", env: { ...process.env, BACKPACK_KEY: pubOf(), BACKPACK_POINTER: pointerFile } });
      else { const pr = await fetch(target.slice(4), { method: "PUT", body: fs.readFileSync(pointerFile) }); if (!pr.ok) throw new Error(`HTTP ${pr.status}`); }
      console.log(`pointer → ${target}`);
    } catch (e) { console.error(`pointer push to ${target} FAILED: ${e.message}`); }
  }
  const q = new URLSearchParams({ i: cid, k: pubOf(), n: s.name, p: node.peerId });
  for (const a of node.addrs) q.append("a", a);
  q.set("s", node.spr);
  for (const u of s.pointers.get) q.append("u", u);
  const link = `backpack://repo?${q}`;
  s.lastLink = link; save(s);
  console.log(`seq ${seq}\nindex ${cid}\n\n${link}\n`);
  try {
    const QR = (await import("qrcode")).default;
    const png = path.join(dir, "repo-qr.png");
    await QR.toFile(png, link, { margin: 2, scale: 6 });
    console.log(`QR: ${png}`);
  } catch { /* qrcode not installed: the link is enough */ }
}

// Check a repo the way a phone would: pointers → index → (deep) APKs. Exit 1 on any failure.
async function verifyCmd() {
  const deep = flag("deep");
  let key, indexCid, pointerUrls = [], node;
  if (args[0]?.startsWith("backpack://")) {
    const q = new URL(args[0].replace("backpack://", "http://x/")).searchParams;
    key = q.get("k"); indexCid = q.get("i"); pointerUrls = q.getAll("u");
    node = fs.existsSync(stateFile) ? load().node : DEFAULT_NODE;
  } else {
    const s = load(); key = pubOf(); indexCid = s.lastIndex; pointerUrls = s.pointers?.get ?? []; node = s.node;
  }
  let fail = 0;
  const bad = (m) => { fail++; console.log(`✗ ${m}`); };
  let best = { seq: -1, index: indexCid, from: "link" };
  for (const u of pointerUrls) {
    try {
      const r = await fetch(u, { cache: "no-store" }); if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const b = openEnvelope(await r.json(), key, "pointer");
      console.log(`✓ pointer ${u}: seq ${b.seq} → ${b.index}`);
      if (b.seq > best.seq) best = { seq: b.seq, index: b.index, from: u };
    } catch (e) { bad(`pointer ${u}: ${e.message}`); }
  }
  let body;
  try {
    body = openEnvelope(JSON.parse((await download(node, best.index)).toString("utf8")), key, "index");
    console.log(`✓ index ${best.index} (from ${best.from}): "${body.name}" v${body.v} seq ${body.seq ?? 0}, ${body.apps.length} apps, signed ${body.created}`);
    if (best.seq >= 0 && (body.seq ?? 0) !== best.seq) bad(`index seq ${body.seq} ≠ pointer seq ${best.seq}`);
  } catch (e) { bad(`index ${best.index}: ${e.message}`); }
  if (body && deep) {
    // every version phones can see (audit BP-14): bytes, SHA-256, pinned signer (required: BP-6), and the
    // APK's own package name and versionCode against the index entry (BP-5)
    if (!Array.isArray(body.apps)) { bad("index: apps is not a list"); body.apps = []; }   // R-8
    for (const a of body.apps) for (const v of (Array.isArray(a?.versions) ? a.versions : (bad(`${a?.id}: versions is not a list`), []))) {
      try {
        if (!/^[0-9a-f]{64}$/.test(v.signer ?? "")) throw new Error("no pinned signer: phones refuse to install it");
        const bytes = await download(node, v.cid);
        if (bytes.length !== v.size) throw new Error(`size ${bytes.length} ≠ ${v.size}`);
        if (sha256hex(bytes) !== v.sha256) throw new Error("SHA-256 mismatch");
        const t = path.join(tmpDir(), "a.apk"); fs.writeFileSync(t, bytes);
        const signer = apkSigner(t);
        if (signer !== v.signer) throw new Error(`signer ${signer.slice(0, 8)}… ≠ pinned ${v.signer.slice(0, 8)}…`);
        const m = badging(t);
        if (m.id !== a.id) throw new Error(`the APK is ${m.id}, the index says ${a.id}`);
        if (m.versionCode !== v.versionCode) throw new Error(`the APK is versionCode ${m.versionCode}, the index says ${v.versionCode}`);
        console.log(`✓ ${a.id} ${v.versionName}: sha256, signer ${signer.slice(0, 8)}…, package and versionCode ok`);
        fs.rmSync(t, { force: true });
      } catch (e) { bad(`${a.id} ${v.versionName}: ${e.message}`); }
    }
  }
  console.log(fail ? `${fail} problem(s)` : "all good");
  process.exit(fail ? 1 : 0);
}

/** Re-upload every kept APK whose blocks are still the 64 KiB default (ADR 0006): bytes from the node,
 *  checked against the pinned SHA-256, uploaded again with 512 KiB blocks. Then `publish`. */
async function rehost() {
  const s = load(); let n = 0;
  for (const a of Object.values(s.apps)) for (const v of a.versions) {
    if (v.blockSize === blockSizeFor(v.size)) continue;  // already uploaded with the right block size
    process.stdout.write(`${a.id} ${v.versionName} (${(v.size / 1e6).toFixed(1)} MB) → `);
    const bytes = await download(s.node, v.cid);
    if (sha256hex(bytes) !== v.sha256) die(`${a.id} ${v.versionName}: the node's copy doesn't match the pinned SHA-256`);
    const t = path.join(tmpDir(), "re.apk"); fs.writeFileSync(t, bytes);
    v.cid = await upload(s.node, t, `${a.id}_${v.versionCode}.apk`, "application/vnd.android.package-archive");
    v.blockSize = blockSizeFor(bytes.length); n++; save(s); console.log(v.cid);
  }
  console.log(n ? `${n} version(s) re-uploaded; run \`backpack publish\`` : "nothing to re-upload");
}

function ls() {
  const s = load();
  console.log(`${s.name}  (key ${pubOf()}, seq ${s.seq ?? 0}, node ${s.node})`);
  for (const a of Object.values(s.apps)) console.log(`  ${a.label.padEnd(20)} ${a.id.padEnd(32)} ${a.versions.map((v) => `${v.versionName}(${v.versionCode})${v.signer ? "" : "!"}`).join(", ")}`);
  if (s.pointers?.get?.length) console.log(`pointers: ${s.pointers.get.join(" ")}`);
  if (s.lastLink) console.log(`\nlast published: ${s.lastLink}`);
}

const cmd = args.shift();
const run = { init, config, add, "import-fdroid": importFdroid, publish, verify: verifyCmd, rehost, ls }[cmd];
if (!run) {
  console.log(`usage: backpack [--repo NAME] <command>
  init --name N [--node URL]
  config [--node URL] [--pointer-get URL]… [--pointer-put rsync:dest|put:URL|exec:CMD]… [--clear-pointers]
  add APK… [--name --summary --description --license]
  import-fdroid <repo URL> --fingerprint SHA256 [--versions N] [--only pkg,pkg] [--allow-sha1] [--allow-stale]
  publish [--force]       (skips when nothing changed)
  verify [backpack://link] [--deep]
  rehost                 re-upload APKs with 512 KiB blocks (faster downloads, ADR 0006)
  ls`);
  process.exit(cmd ? 1 : 0);
}
await run();
