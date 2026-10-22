#!/usr/bin/env node
// backpack — publish Android apps to a repository that lives in Logos Storage.
//
// The APKs, their icons and the repository index are all Storage objects (content-addressed CIDs).
// The index is signed with the repo's ed25519 key, so a phone that trusts the key accepts any
// newer index from the same repo, whichever node served it. A repo link names the index CID, the
// key, and the Storage node to fetch from:
//   backpack://repo?i=<indexCID>&k=<pubkey>&n=<name>&p=<peerId>&a=<multiaddr>[&a=…]&s=<spr>
//
//   backpack init --name "My backpack" [--node http://[…]:8480]
//   backpack add <file.apk>…          upload APK(s) + icon, record the version
//   backpack publish                  upload the signed index, print the repo link (+ QR png)
//   backpack ls
//
// State: ~/.backpack/<repo>/repo.json (+ key.pem, 0600). The node is a Logos Storage node's REST API
// (logos-storage `--api-*`); no auth — use one reachable only by you (mesh/localhost).
import { createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign } from "node:crypto";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const DEFAULT_NODE = "http://[fdb0:9afc:a5ef:f167:7ad7:68b7:aca0:b9fa]:8480"; // VPS storage node, mesh-only REST
const AAPT = process.env.AAPT ?? findAapt();
const KEEP_VERSIONS = 3;

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(`--${name}`); if (i < 0) return def; const v = args[i + 1]; args.splice(i, 2); return v; };
const repoName = opt("repo", "default");
const dir = path.join(os.homedir(), ".backpack", repoName);
const stateFile = path.join(dir, "repo.json"), keyFile = path.join(dir, "key.pem");
const b64url = (b) => Buffer.from(b).toString("base64url");
const die = (m) => { console.error(`backpack: ${m}`); process.exit(1); };

function findAapt() {
  const bt = path.join(os.homedir(), "Android/Sdk/build-tools");
  try { const v = fs.readdirSync(bt).sort().reverse()[0]; return path.join(bt, v, "aapt2"); } catch { return "aapt2"; }
}
function load() { if (!fs.existsSync(stateFile)) die(`no repo "${repoName}" — run: backpack init --name "…"`); return JSON.parse(fs.readFileSync(stateFile, "utf8")); }
function save(s) { fs.writeFileSync(stateFile, JSON.stringify(s, null, 2)); }
function pubOf() { return createPublicKey(createPrivateKey(fs.readFileSync(keyFile))).export({ format: "jwk" }).x; }

// ── Storage node REST ────────────────────────────────────────────────────────
async function upload(node, file, name, mime) {
  const body = fs.readFileSync(file);
  const r = await fetch(`${node}/api/storage/v1/data`, {
    method: "POST", body,
    headers: { "Content-Type": mime, "Content-Disposition": `attachment; filename="${name}"` },
  });
  if (!r.ok) die(`upload ${name}: HTTP ${r.status} ${await r.text()}`);
  return (await r.text()).trim();
}
async function nodeInfo(node) {
  const r = await fetch(`${node}/api/storage/v1/debug/info`);
  if (!r.ok) die(`node ${node}: HTTP ${r.status}`);
  const j = await r.json();
  return { peerId: j.id, addrs: j.addrs, spr: j.spr };
}

// ── APK metadata (aapt2 dump badging) ────────────────────────────────────────
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

// The badging icon is often an adaptive-icon XML; its resource also has per-density rasters
// (png/webp). Take the largest one.
function rasterIcon(apk, icon) {
  if (!icon || /\.(png|webp)$/.test(icon)) return icon;
  const dump = execFileSync(AAPT, ["dump", "resources", apk], { encoding: "utf8", maxBuffer: 256 << 20 });
  const blocks = dump.split(/\n(?=\s*resource 0x)/);
  const block = blocks.find((b) => b.includes(`(file) ${icon}`));
  if (!block) return null;
  const order = ["xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", ""];
  const files = [...block.matchAll(/\((\w*)[^)]*\) \(file\) (\S+\.(?:png|webp))/g)].map((m) => [m[1], m[2]]);
  files.sort((a, b) => order.indexOf(a[0]) - order.indexOf(b[0]));
  return files[0]?.[1] ?? null;
}

// ── commands ─────────────────────────────────────────────────────────────────
async function init() {
  const name = opt("name", null) ?? die("--name is required");
  const node = opt("node", DEFAULT_NODE);
  if (fs.existsSync(stateFile)) die(`repo "${repoName}" already exists (${dir})`);
  fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
  const { privateKey } = generateKeyPairSync("ed25519");
  fs.writeFileSync(keyFile, privateKey.export({ format: "pem", type: "pkcs8" }), { mode: 0o600 });
  save({ name, node, apps: {} });
  console.log(`repo "${name}" created in ${dir}\nkey ${pubOf()}\nnode ${node}`);
}

async function add() {
  const s = load();
  if (!args.length) die("add <file.apk>…");
  for (const apk of args) {
    const m = badging(apk);
    if (!m.id || !m.versionCode) die(`${apk}: not an APK aapt2 can read`);
    const bytes = fs.readFileSync(apk);
    const sha256 = createHash("sha256").update(bytes).digest("hex");
    process.stdout.write(`${m.id} ${m.versionName} (${m.versionCode}) ${(bytes.length / 1e6).toFixed(1)} MB → uploading… `);
    const cid = await upload(s.node, apk, `${m.id}_${m.versionCode}.apk`, "application/vnd.android.package-archive");
    let iconCid = s.apps[m.id]?.icon ?? null;
    if (m.icon) {
      const ext = m.icon.endsWith(".webp") ? "webp" : "png";
      const tmp = path.join(os.tmpdir(), `backpack-icon-${process.pid}.${ext}`);
      fs.writeFileSync(tmp, execFileSync("unzip", ["-p", apk, m.icon], { maxBuffer: 16 << 20 }));
      iconCid = await upload(s.node, tmp, `${m.id}.${ext}`, `image/${ext}`);
      fs.rmSync(tmp);
    }
    const app = (s.apps[m.id] ??= { id: m.id, label: m.label, icon: null, versions: [] });
    app.label = m.label; app.icon = iconCid;
    app.versions = [{ versionCode: m.versionCode, versionName: m.versionName, size: bytes.length, sha256, cid, minSdk: m.minSdk, abis: m.abis, added: new Date().toISOString() },
      ...app.versions.filter((v) => v.versionCode !== m.versionCode)].sort((a, b) => b.versionCode - a.versionCode).slice(0, KEEP_VERSIONS);
    console.log(cid);
  }
  save(s);
  console.log("recorded; run `backpack publish` to make it visible");
}

async function publish() {
  const s = load();
  const node = await nodeInfo(s.node);
  const body = JSON.stringify({
    v: 1, name: s.name, key: pubOf(), created: new Date().toISOString(),
    nodes: [{ peerId: node.peerId, addrs: node.addrs, spr: node.spr }],
    apps: Object.values(s.apps).sort((a, b) => a.label.localeCompare(b.label)),
  });
  const sig = sign(null, Buffer.from(body), createPrivateKey(fs.readFileSync(keyFile))).toString("base64url");
  const tmp = path.join(os.tmpdir(), `backpack-index-${process.pid}.json`);
  fs.writeFileSync(tmp, JSON.stringify({ backpack: 1, body, sig }));
  const cid = await upload(s.node, tmp, "backpack-index.json", "application/json");
  fs.rmSync(tmp);
  const q = new URLSearchParams({ i: cid, k: pubOf(), n: s.name, p: node.peerId });
  for (const a of node.addrs) q.append("a", a);
  q.set("s", node.spr); // the phone bootstraps its Storage node off this record on first contact
  const link = `backpack://repo?${q}`;
  s.lastIndex = cid; s.lastLink = link; save(s);
  console.log(`index ${cid}\n\n${link}\n`);
  try {
    const QR = (await import("qrcode")).default;
    const png = path.join(dir, "repo-qr.png");
    await QR.toFile(png, link, { margin: 2, scale: 6 });
    console.log(`QR: ${png}`);
  } catch { /* qrcode not installed: the link is enough */ }
}

function ls() {
  const s = load();
  console.log(`${s.name}  (key ${pubOf()}, node ${s.node})`);
  for (const a of Object.values(s.apps)) console.log(`  ${a.label.padEnd(20)} ${a.id.padEnd(32)} ${a.versions.map((v) => `${v.versionName}(${v.versionCode})`).join(", ")}`);
  if (s.lastLink) console.log(`\nlast published: ${s.lastLink}`);
}

const cmd = args.shift();
const run = { init, add, publish, ls }[cmd];
if (!run) { console.log("usage: backpack [--repo NAME] init --name N [--node URL] | add APK… | publish | ls"); process.exit(cmd ? 1 : 0); }
await run();
