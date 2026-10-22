# 🎒 Backpack

Install Android apps from repositories that live in **Logos Storage**: a tiny F-Droid, where the
APKs, their icons and the repository index are all Storage objects fetched by CID.

- `cli/` — `backpack`, the desktop side: uploads APKs + icons to a Storage node, keeps the repo
  state, publishes a **signed index** and prints the repo link.
- `mobile/` — the Android app (Expo/RN): add a repo from a `backpack://` link, see its apps,
  install/update them. Logos Storage runs on the phone (libstorage, the same fetch client Scala ships).
- `probe/` — desktop harnesses that run the phone's exact Storage call sequence against the phone's
  libstorage commit, plus `e2e.sh` (index fetch → signature check → APK fetch → SHA-256).

## How it works

```
desktop: backpack add app.apk ──REST──▶ Storage node (VPS, public p2p, mesh-only REST)
         backpack publish     ──REST──▶ index.json (signed ed25519) → CID
                              ──────▶ backpack://repo?i=<indexCID>&k=<key>&n=<name>&p=<peer>&a=<addr>&s=<spr>
phone:   open link → libstorage: bootstrap off the SPR, dial the node, fetch the index CID
         verify the signature with k → list apps → fetch icon/APK CIDs → SHA-256 vs index
         → Android PackageInstaller (system confirmation)
```

- **Trust:** the repo *is* its ed25519 key. An index is accepted only if that key signed it, and
  a newer index replaces an older one only when it's from the same key and newer (`created`). Each
  APK's SHA-256 is in the signed index, so a Storage node can't swap the bytes.
- **Index file:** `{"backpack":1,"body":"<json>","sig":"<b64url>"}`. The body is
  `{v,name,key,created,nodes:[{peerId,addrs,spr}],apps:[{id,label,icon,versions:[{versionCode,
  versionName,size,sha256,cid,minSdk,abis}]}]}`. The newest 3 versions per app are kept.
- **Installing:** a PackageInstaller session (no FileProvider). Android asks once to allow "install
  unknown apps" for Backpack, and shows its normal confirmation for each install or update.
  Updates need the same signing key as the installed app, as always on Android.

## Use

```sh
cd cli && npm install
./backpack.mjs init --name "My backpack"   # --node http://…:8480 (a Storage node's REST API)
./backpack.mjs add path/to/app.apk …
./backpack.mjs publish                     # prints the link + ~/.backpack/<repo>/repo-qr.png
./backpack.mjs ls
```

Open the printed `backpack://` link on the phone, or scan the QR, to add the repo. After a new
`publish`, open the new link: Backpack recognises the repo by its key and moves to the newer index.

## Proof-of-concept limits (next steps)

- **Updating a repo needs a new link.** The link pins an index CID. Next: a mutable pointer, e.g.
  the signed index CID announced on a Logos Delivery topic derived from the key, or a small "latest"
  record the app polls.
- **One Storage node.** The index can list several nodes; the CLI writes one. Hubs that pin the
  repo's CIDs (cache-on-publish) would make it survive the node going away.
- **No background update checks or notifications.** Refresh is pull-to-refresh.
- **arm64 phones only** (libstorage ships for arm64).
- **The phone's libstorage is Scala's build** (nim-codex 6af2049). The JNI glue here carries a fix
  Scala doesn't have yet: a call that timed out freed its response while libstorage still held it
  (use-after-free on a late callback).

## Build the app

```sh
cd mobile && npm install
native/logosstorage/build-jni.sh          # JNI glue (+ copies libstorage.so from Scala's build)
npx expo prebuild --platform android --clean
echo "sdk.dir=$HOME/Android/Sdk" > android/local.properties
sed -i '0,/^android {/s//android {\n    lint { checkReleaseBuilds false; abortOnError false }/' android/app/build.gradle
cd android && ./gradlew assembleRelease     # signed with BACKPACK_* from ~/.gradle/gradle.properties
```

Licence: MIT OR Apache-2.0.
