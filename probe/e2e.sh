#!/usr/bin/env bash
# e2e.sh <backpack:// link> — everything the phone does except Android's installer, on the desktop:
# phone-path fetch of the index (libstorage 6af2049), the app's own parse/verify code (src/repo.ts
# logic, run under node), then a phone-path fetch of the first app's APK + SHA-256 against the index.
set -euo pipefail
LINK=$1; LSN=~/logos-storage-nim; HERE=$(cd "$(dirname "$0")" && pwd); OUT=${TMPDIR:-/tmp}/backpack-e2e; mkdir -p $OUT
# No eval (audit BP-17): node prints one value per line, read into an array; every use is quoted.
mapfile -t F < <(node -e '
const q=new URL(process.argv[1].replace("backpack://","http://x/")).searchParams;
for (const v of [q.get("i"),q.get("k"),q.get("p"),q.getAll("a")[0],q.get("s")]) console.log(String(v??"").replace(/[\r\n]/g,""));' "$LINK")
CID=${F[0]}; KEY=${F[1]}; PEER=${F[2]}; ADDR=${F[3]}; SPR=${F[4]}
run() { rm -rf ~/.backpack-probe; LD_LIBRARY_PATH=$LSN/build "$HERE/phonepath" "$1" "$2" "$SPR" "$PEER" "$ADDR" 2>&1 | grep -E "^(start|connect|fetch|download)" | cut -c1-90; }
echo "== index $CID"; run "$CID" "$OUT/index.json"
node --input-type=module -e '
import fs from "fs"; import { ed25519 } from "'$HERE'/../mobile/node_modules/@noble/curves/ed25519.js";
const b=(s)=>Buffer.from(s,"base64url"); const f=JSON.parse(fs.readFileSync(process.argv[1],"utf8"));
const ok=ed25519.verify(b(f.sig), Buffer.from(f.body), b(process.argv[2])); const i=JSON.parse(f.body);
console.log("signature", ok ? "VALID" : "INVALID", "| repo", JSON.stringify(i.name), "|", i.apps.map(a=>a.label+" "+a.versions[0].versionName).join(", "));
const a=i.apps.find(a=>a.id==="xyz.vpavlin.backpack")??i.apps[0]; fs.writeFileSync(process.argv[3], a.versions[0].cid+" "+a.versions[0].sha256+"\n");
' $OUT/index.json "$KEY" $OUT/pick
read APK SHA < $OUT/pick; echo "== apk $APK"; run "$APK" $OUT/app.apk
GOT=$(sha256sum $OUT/app.apk | cut -d" " -f1); [ "$GOT" = "$SHA" ] && echo "sha256 MATCHES index ($SHA)" || { echo "sha256 MISMATCH $GOT != $SHA"; exit 1; }
