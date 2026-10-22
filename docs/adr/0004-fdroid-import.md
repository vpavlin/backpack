# 0004. Import an F-Droid repo only after verifying its signed index

**Status:** accepted (2026-10-22)

## Decision

`backpack import-fdroid <repo URL> --fingerprint <sha256>` mirrors an F-Droid repo into a Backpack repo:

1. **Verify first.**
   - Download `index-v1.jar`.
   - Verify its JAR signature (`jarsigner -verify`).
   - Require the signing certificate's SHA-256 to equal the given fingerprint: F-Droid's repo
     fingerprint, the one in its share links.
   - Read `index-v1.json` only from inside the verified JAR. Without a matching fingerprint
     nothing is imported.
2. **For each app**, take the newest `--versions N` (default 1) versions, then for each one:
   - download the APK;
   - require its SHA-256 to equal the hash in the verified index;
   - read its signing certificate with `apksigner`, and require it to match the index's `signer`
     where the index has one;
   - upload it to Storage.

   Versions already in the Backpack repo (same package, versionCode and SHA-256) are skipped, so
   re-running only adds what is new.
3. **Metadata comes from the F-Droid index:** name, summary, description, licence, icon.
   - The icon comes from the repo's `icons-640/` with fallbacks.
   - If none is found, the icon is extracted from the APK.
4. `backpack publish` then signs the new index and pointer as usual.

## Caveat: SHA-1

F-Droid signs `index-v1.jar` with SHA1withRSA, and current JDKs refuse it as a weak algorithm.

- The F-Droid client still accepts that signature. The import re-enables the legacy algorithms
  for that one `jarsigner` call only (a temporary `java.security` override), so it is no weaker
  than F-Droid itself.
- Each APK is still checked against the SHA-256 in that index, and against its own signer.
- Moving to the v2 index (`entry.jar` + `index-v2.json`, SHA-256) is the better long-term input.

## Consequences

- Trust chains from the F-Droid repo key to the Backpack repo key. Whoever runs the import vouches
  for the content, and the original APK signers stay pinned per version ([0002](0002-security-model.md)).
- The APKs are byte-identical to F-Droid's: same SHA-256, same app signature. A phone can move
  between the two sources for the same app.
- It runs as a cron job on the box that publishes the F-Droid repo.
