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

## Not yet

- A setting to limit automatic downloads to Wi-Fi.
- Background updates while the app is closed (WorkManager).
