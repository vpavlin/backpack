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

A repo is three kinds of things. Only the first one can ever touch the web, and it is signed:

| What | Size | Carried by | Trust |
|---|---|---|---|
| **Update note** (the pointer): repo key, `seq`, index CID | ~300 B | **Logos Delivery** topic `/backpack/1/<key>/json` (live, and from Store on start); optionally each HTTPS address in the link (Settings, off by default) | signed by the repo key; a lower `seq` than the phone already has is refused |
| **Index**: the app list, descriptions, versions, SHA-256s, pinned signers, APK CIDs | KBs | **Logos Storage**, by the CID in the note | signed by the repo key; `seq` must equal the note's |
| **APKs and icons** | MBs | **Logos Storage**, by CID (512 KiB blocks) | SHA-256 from the index; the APK's signing certificate must be the pinned one (and the installed app's) |

```
publisher: backpack import-fdroid <repo> --fingerprint …   (or: add app.apk)   ──REST──▶ Storage node (VPS)
           backpack publish  → signed index (seq N) → CID                    ──REST──▶ Storage node
                             → signed note {key, seq N, index CID}           ──rsync──▶ VPS ──Delivery──▶ topic
                               (re-sent every 12 h from the VPS so Store keeps it; ops/pointer-announce)
phone:     backpack:// link once (key + first index + Storage node + optional HTTPS addresses)
           on open: Loam shared node → topic (live + Store) [+ HTTPS if allowed] → newest valid note
                    → index CID → Logos Storage → verify index → apps
           updates: installed app + newer version + same signing key → download → verify → install
           (Settings: "Install app updates automatically")
```

Decisions are in **[docs/adr](docs/adr/)**:
- [0001](docs/adr/0001-repos-on-logos-storage.md): a repo is an ed25519 key; all content lives in Logos Storage.
- [0002](docs/adr/0002-security-model.md): the security model (signed index, monotonic seq, SHA-256, pinned APK signers).
- [0003](docs/adr/0003-updates-signed-pointer.md): updates by a signed note (pointer) that can be served from anywhere.
- [0004](docs/adr/0004-fdroid-import.md): F-Droid import, only through a verified index.
- [0005](docs/adr/0005-phone-libstorage.md): the phone's libstorage build (PR #1540) and why.
- [0006](docs/adr/0006-download-speed-block-size.md): download speed, 512 KiB blocks, the progress bar, mirrors.
- [0007](docs/adr/0007-decentralising-the-pointer.md): the note over Logos Delivery; a chain later, for keys.
- [0008](docs/adr/0008-automatic-updates.md): automatic updates, details, uninstall, cancel, sources, Settings.

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

- **Delivery needs the Loam app** on the phone (its shared node). Without it, and with the web switch
  off, a repo only moves forward when you open a newer link.
- **The note is announced from one box** (the VPS hub). Anyone holding the note can re-send it (it is
  signed), so mirrors can take over; that is not built yet.
- **No key rotation yet.** A new repo key means a new link (ADR 0002; a chain is the candidate, ADR 0007).
- **Download speed.** Storage's block exchange is round-trip bound: 200-500 KB/s from the VPS with
  512 KiB blocks. More nodes serving a repo (mirrors) should help a lot (ADR 0006).
- **arm64 phones only.** The x86_64 build is for emulator tests (`BACKPACK_EMU=1`); Loam's node does not
  run on the emulator.

## Build the app

```sh
git submodule update --init          # loam-transport (Delivery via the Loam app)
cd mobile && npm install
native/logosstorage/build-jni.sh          # JNI glue; libstorage.so per native/logosstorage/PROVENANCE (ADR 0005)
npx expo prebuild --platform android --clean
echo "sdk.dir=$HOME/Android/Sdk" > android/local.properties
sed -i '0,/^android {/s//android {\n    lint { checkReleaseBuilds false; abortOnError false }/' android/app/build.gradle
cd android && ./gradlew assembleRelease     # signed with BACKPACK_* from ~/.gradle/gradle.properties
```

Licence: MIT OR Apache-2.0.
