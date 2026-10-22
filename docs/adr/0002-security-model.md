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
