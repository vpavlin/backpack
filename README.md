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
desktop:  backpack add app.apk | import-fdroid <repo> --fingerprint …   ──REST──▶ Storage node
          backpack publish  → signed index (seq N) → CID                ──REST──▶ Storage node
                            → signed pointer {key, seq N, index CID}   ──rsync/PUT──▶ any HTTPS host(s)
phone:    backpack:// link (key + first index + nodes + pointer URLs)
          on start: newest valid pointer (signature, seq > stored) → index CID → libstorage fetch
          verify index (signature, key, seq ≥ stored, = pointer seq) → apps
          install: fetch APK by CID → SHA-256 = index → signer = pinned signer (+ = installed app's)
          → Android PackageInstaller
```

Decisions are in **[docs/adr](docs/adr/)**:
- [0001](docs/adr/0001-repos-on-logos-storage.md): a repo is an ed25519 key; all content lives in Logos Storage.
- [0002](docs/adr/0002-security-model.md): the security model (signed index, monotonic seq, SHA-256, pinned APK signers).
- [0003](docs/adr/0003-updates-signed-pointer.md): updates by a signed pointer that can be served from anywhere.
- [0004](docs/adr/0004-fdroid-import.md): F-Droid import, only through a verified index.
- [0005](docs/adr/0005-phone-libstorage.md): the phone's libstorage build (PR #1540) and why.

## Use

```sh
cd cli && npm install
./backpack.mjs init --name "My backpack"            # key in ~/.backpack/default/key.pem — back it up
./backpack.mjs config --pointer-get https://example.org/backpack/<key>.json \
                      --pointer-put rsync:host:/srv/www/backpack/<key>.json
./backpack.mjs add path/to/app.apk …                 # records sha256 + signing cert per version
./backpack.mjs import-fdroid http://host/fdroid/repo --fingerprint <repo cert sha256>
./backpack.mjs publish                               # seq+1, index → Storage, pointer → hosts, link + QR
./backpack.mjs verify --deep                         # check it all as a phone would
./backpack.mjs rehost                                # re-upload old APKs with 512 KiB blocks (ADR 0006)
```

Open the printed `backpack://` link on the phone, or scan the QR, once. After that the phone
follows the pointer: every `publish` reaches it on the next app start or pull-to-refresh.

**Tests:**
- `node cli/test/envelope.test.mjs` runs the CLI against the signing vectors.
- `node --experimental-strip-types mobile/test/envelope-vectors.test.ts` runs the phone's verifier
  against the same vectors.
- `cli/test/make-vectors.mjs` regenerates them.

## Limits (next steps)

- **Pointers are HTTPS.** They're signed, so the host is trusted only for liveness. The next step
  is a Logos Delivery topic per repo key (ADR 0003).
- **No key rotation yet.** A new repo key means a new link (ADR 0002).
- **Download speed.** libstorage (PR #1540) downloads at about 100–200 KB/s from the VPS node; a
  50 MB APK takes minutes.
- **arm64 phones only.** The x86_64 build is for emulator tests (`BACKPACK_EMU=1`).

## Build the app

```sh
cd mobile && npm install
native/logosstorage/build-jni.sh          # JNI glue; libstorage.so per native/logosstorage/PROVENANCE (ADR 0005)
npx expo prebuild --platform android --clean
echo "sdk.dir=$HOME/Android/Sdk" > android/local.properties
sed -i '0,/^android {/s//android {\n    lint { checkReleaseBuilds false; abortOnError false }/' android/app/build.gradle
cd android && ./gradlew assembleRelease     # signed with BACKPACK_* from ~/.gradle/gradle.properties
```

Licence: MIT OR Apache-2.0.
