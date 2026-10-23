# 0002. Signed index, monotonic sequence, per-APK SHA-256 and pinned signer

**Status:** accepted (2026-10-22)

## Threats

| Who | Could try to | Stopped by |
|---|---|---|
| A Storage node, a relay, or anyone on the path | serve a different APK under a known CID | CIDs are content hashes; also the APK's SHA-256 in the signed index |
| The same | serve a forged index | the ed25519 signature over the index body, checked against the key from the link |
| A pointer host or mirror | point the phone at an old index with a known-vulnerable version (rollback) | `seq` in the signed index and pointer; the phone never accepts a lower `seq` than it has seen |
| The same | hide updates (freeze) | partly: multiple pointer sources ([0003](0003-updates-signed-pointer.md)); the UI shows when the repo was last confirmed |
| A repo key holder, or a stolen repo key | replace an app with one signed by a different developer key | the index pins each version's **APK signing certificate**; the phone checks the downloaded APK's signer against it, and against the installed app's signer, before handing it to Android |
| Anyone | trick the user into a different repo | the repo is its key; adding a repo shows its name and key prefix; a second repo with the same key replaces the first only with a higher `seq` |

## Decision

**The index** is a Storage object `{"backpack":1,"body":"<json>","sig":"<b64url>"}`.
- `sig` is ed25519 over the exact UTF-8 bytes of `body`, so no JSON canonicalisation is needed.
- `body` = `{v:2, name, key, seq, created, nodes:[{peerId,addrs,spr}], pointers:[url], apps:[…]}`.
- The phone rejects an index if:
  - the signature fails;
  - `body.key` is not the repo key;
  - `seq` is lower than the highest seq already accepted for this repo (v1 indexes have no seq and count as 0).

**Every version** in the index carries `sha256`, `size`, `cid` and `signer`, the SHA-256 of the
APK's signing certificate as `apksigner --print-certs` prints it. Before installing, the phone:
1. downloads by CID and checks the size and SHA-256;
2. reads the APK's signing certificate (PackageManager.getPackageArchiveInfo with
   GET_SIGNING_CERTIFICATES) and requires it to equal `signer`;
3. if the package is already installed, compares the installed signer too. A mismatch is reported
   before Android refuses the update, since only an uninstall gets past that.

**Keys:**
- The repo key lives in `~/.backpack/<repo>/key.pem` (0600) on the publishing machine. Lose it and
  phones can only move to a new repo by a new link.
- APK signing keys are separate, per app, and never touch Backpack.

## Not (yet) covered

- **Key rotation:** for now a new key means a new repo. Next step: the old key signs a hand-over to
  the new one.
- **Freeze attacks**, if every pointer source is captured. The phone can only show how stale its
  view is.
- **Transparency** (a public log of every index ever signed).

## Amendment 2026-10-23: independent review

proteus/auditor reviewed 4403ad9 (report: 22 findings; the envelope core, rollback handling and the JNI
late-callback fix held up). What changed in response:

| Finding | Fix |
|---|---|
| BP-1 import-fdroid used index names as local paths | generated temp names; remote names must match a safe pattern; tested with a malicious signed repo |
| BP-2 root announcer followed symlinks, sent any `*.json` | `send.py`: regular files only (`O_NOFOLLOW`), key-named, ≤ 4 KiB, signature verified with the key in the name; no shell |
| BP-3 import re-enabled every disabled JAR algorithm (MD5) | import the F-Droid **v2** index: `entry.jar` verifies with the JDK's stock policy; v1 is no longer read |
| BP-4 a malformed signed index persisted and crashed the app | `mobile/src/schema.ts` validates every field and caps every list before anything is stored; stored repos are re-validated on load; an error boundary offers a way out; an index envelope must carry no `kind` |
| BP-5 APK identity not bound to the index | the APK's package name and versionCode must equal the index's (phone: `apkInfo`; CLI import and `verify --deep`) |
| BP-6 a version without a signer skipped the signer checks | a pinned signer is required, by the schema and by `install()` |
| BP-7 index strings as file paths on the phone | package-name and integer patterns make them path-safe |
| BP-8 "no user action" requested for every install | only for an update of an app Backpack itself installed |
| BP-9 external links added repos silently and could re-point a known repo | a confirmation with the full publisher key; nothing unsigned from a link is merged into a known repo |
| BP-10/11/12/22 unbounded messages, URLs, peers, sizes | caps (8 candidates × 8 KiB per Delivery message, 4 HTTPS pointer URLs to public hosts only, 8 nodes × 4 addresses, 1 MB index, 500 apps, 5 versions, 512 KiB icons); one refresh trigger per repo per 30 s; 30 s connect timeout |
| BP-15 static install-status broadcast before API 33 | a random action per install session |
| BP-16 JNI null/overflow paths | allocation and JNI-string checks, size guard, bounded address array, short timeouts for connect/cancel/delete |
| BP-17 `probe/e2e.sh` eval | no eval |
| BP-18 `source` opened as any URL | only http(s) |
| BP-19/20/21 quiet-path alerts, cancelled prompt not remembered, removed repo still watched | fixed |

Accepted, not changed: BP-13 (the phone relies on libstorage's content addressing for the index and
icons; the index is also signed, and APKs are hashed), and `storageNew` leaking a context on timeout
(it can't be cancelled; it happens once per process at most).

