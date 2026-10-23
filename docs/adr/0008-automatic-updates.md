# 0008. Automatic updates, app details, uninstall

**Status:** accepted (2026-10-23)

## Decision

- **On every start**, and when the app comes back to the foreground more than 15 minutes after the
  last check, Backpack follows every repo's pointer (ADR 0003), then **downloads and installs updates
  for installed apps**, one at a time, without a tap.
- **An update is offered only when the new version is signed by a key the installed app already
  has.** Android would refuse anything else, and a repo that lists a package under a different key
  is either a fork or a forgery. Across repos, the highest `versionCode` with a matching key wins.
- **Android confirms each install**, except for apps Backpack itself installed: on Android 12+ those
  update without a prompt (`setRequireUserAction(USER_ACTION_NOT_REQUIRED)`).
- **Each app has a detail screen**: summary, description, licence, source link, minimum Android,
  ABIs, versions in the repo, and the publisher:
  - repository name and key, and the APK's pinned signing certificate;
  - whether the installed app has the same signing key (green) or a different one (red);
  - which app installed it;
  - a warning when another repo lists the same package under a different signing key.
- **Uninstall** opens Android's own uninstall dialog (`REQUEST_DELETE_PACKAGES`).
- Each repo header shows its publisher key, so two repos with the same name can be told apart.

- **Cancel** (0.2.4): while an app downloads, its button is Cancel. It calls libstorage's
  `storage_download_cancel` from a second thread (the storage thread is the one blocked in the
  download), stops the retries, deletes the partial file and drops the CID's blocks. A cancelled
  *automatic* update is remembered per package and version and not started again by itself; the
  Update button still works.
- **Where updates come from** (0.2.4): every source is asked at once (Delivery, each HTTPS URL).
  Under each repo the app shows what each one said and which one the current index came from, e.g.
  `#7 came from Delivery (store) · Delivery (store) ✓ #7 · data.vpavlin.xyz ✗ timed out`.
  "store" means the pointer was signed before Backpack started listening, so it came from Store;
  "live" means it arrived while listening.

- **Settings** (0.2.5), on the device:
  - *Install app updates automatically* (default on). Repos are always checked on open; this switch
    only decides whether updates for installed apps are then downloaded and installed by themselves.
  - *Also check repositories' web addresses* (default **off**). Off: update notes come only over Logos
    Delivery (ADR 0007). Only the signed note (seq + index CID) ever came over HTTP; the index, icons
    and APKs always come from Logos Storage by CID. With it off and no Loam app, a repo stays at the
    version its link pointed to until it is opened from a newer link.
- **The CLI skips `publish` when nothing changed** (`--force` to publish anyway), so a routine run
  doesn't bump `seq` and make every phone re-fetch the same index.

## Not yet

- A setting to limit automatic downloads to Wi-Fi.
- Background updates while the app is closed (WorkManager).
