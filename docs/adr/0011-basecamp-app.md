# 0011. Backpack for Basecamp: manage repos and memberships on the desktop

**Status:** proposed (2026-10-27)

## Context

Today a Backpack repo is run from a terminal on one box: `cli/backpack.mjs` with the repo state and keys in
`~/.backpack/<repo>`, plus the publisher agent that mirrors F-Droid repos into it. That works for us and
nobody else. With private repos (ADR 0009) and LEZ memberships (ADR 0010, laptop/SPEL) there is more to
manage: read keys and invite links, key rotation, who is a member, renewals, a treasury, a key bundle per
period. And a member needs somewhere to register and pay, which a phone can't do: it has no LEZ node.

Basecamp is where Logos desktop apps live. It already has a Logos Storage node (host-owned on 0.3),
Delivery through loam_core, the Loam identity, and Keycard.

## Decision

One Basecamp app, **Backpack**, in two modules, with two roles in one window.

### Modules

- **`backpack_core`** (core, C++, universal interface): everything the CLI does, as methods, plus
  membership. No UI, so it also runs headless under logosctl (a publisher box, the VPS).
  - repos: `createRepo(name, private)`, `listRepos()`, `repo(key)`, `addApk(key, path)`,
    `importFdroid(key, url, fingerprint, versions)`, `publish(key)`, `verify(key, deep)`, `link(key)`
    (the invite link and QR, private ones carry the read key), `rotateReadKey(key)`, `removeVersion`;
  - health: `health(key)` = the newest pointer each source serves, whether Delivery has it, whether the
    Storage hosts still hold every CID of the current index;
  - membership (ADR 0010): `members(key)`, `treasury(key)`, `epoch(key)`, `publishBundle(key)`;
    as a member: `register(repoKey, devicePubkey, periods)`, `renew(...)`, `myMemberships()`.
  - Events: `published`, `progress` (uploads), `membershipChanged`.
  The format code is the CLI's, ported: envelopes (ADR 0002), private repos (ADR 0009: AES-256-GCM,
  bpe1 files), checked against the same test vectors (`cli/test/*vectors.json`) so the CLI, the phone and
  the desktop can't drift.
- **`backpack_ui`** (ui_qml, Logos design system): the window. Talks only to `backpack_core`, never
  blocking (async calls with callbacks; no `callModule` from QML that waits).

### Roles in the window

- **Publisher** (repos this desktop holds the signing key for):
  - a list of repos with public/private badge, seq, last publish, health dot;
  - a repo page: apps and versions (add an APK by file picker, import from F-Droid), Publish, the
    invite link and QR (with a "this is the read key" warning on private ones), Rotate read key;
  - for a members repo: members and their expiry, the treasury balance, the current period and whether
    its key bundle is published.
- **Member** (repos I subscribe to): register or renew from the desktop wallet, my devices (each phone's
  member key, added by scanning the QR Backpack on the phone shows), expiry reminders.
  An optional "register privately" path funds a fresh account from LEZ's shielded pool. laptop/SPEL showed
  live on v0.2.5 that the shield and deshield legs are public and trivially linkable by amount and timing,
  so the app must deshield a **standard denomination** (not `price × periods`) and put a **delay or
  batching** between shielding and deshielding; an immediate same-amount hop is false privacy (ADR 0010,
  "Funding privacy — what v0.2.5 actually provides").

### Where things live

- **Signing key and read keys:** in the module's data directory, mode 600, as the CLI keeps them, at
  first. Later: the repo signing key on Keycard (sign the index and pointer on the card; they are small).
  Import/export of a CLI repo directory, so `~/.backpack/<repo>` moves into the app and back.
- **Storage:** uploads go to a node phones can reach. A desktop behind NAT can't serve phones (see
  "Storage NAT → hub cache"), so `backpack_core` uploads through `storage_gateway` to the VPS node, as
  the CLI does today, and uses the host's own node only for verify and caching. The node is a setting.
- **Pointer:** sent over Delivery through loam_core at every publish and every 12 hours while the module
  runs (done in 0.4.0, see ADR 0007); written to web hosts only if the repo lists any.
- **LEZ:** through laptop/SPEL's surface for the `backpack_access` program (register, renew, list members),
  either a Basecamp module or a library `backpack_core` links. Wallet and signing follow that program's
  LEZ client.

### Publishing stays possible from the CLI

The CLI and the app share the state format, so a repo can move between them. The publisher agent keeps
mirroring the F-Droid repos with the CLI; repos vpavlin runs by hand move to the app.

### Platforms

Linux x86_64 and arm64 from the first release (vpavlin's Duet is arm64); macOS when the CI path for the
other modules covers it.

## Order

1. `backpack_core` with the repo half (create, add, import, publish, verify, link, rotate) and the vectors;
   headless test under logosctl against the lab repo.
2. `backpack_ui` publisher view.
3. Health.
4. Membership, once ADR 0010's program has the multi-device record and the bundle format is fixed.

## Not decided yet

- Whether the app should also *install* to a phone (send an APK over Loam/BLE). Not needed: the phone
  installs from the repo.
- Multiple publishers per repo (a shared signing key or a key per publisher with delegation).
