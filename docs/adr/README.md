# Architecture decision records

| # | Decision | Status |
|---|---|---|
| [0001](0001-repos-on-logos-storage.md) | A repo is an ed25519 key; everything it serves lives in Logos Storage | accepted |
| [0002](0002-security-model.md) | Signed index, monotonic sequence, per-APK SHA-256 and pinned signer | accepted |
| [0003](0003-updates-signed-pointer.md) | Updates: a signed pointer to the newest index, served from anywhere | accepted |
| [0004](0004-fdroid-import.md) | Import an F-Droid repo only after verifying its signed index | accepted |
| [0005](0005-phone-libstorage.md) | The phone's libstorage: fresh build, PR #1540 download fix | accepted |
| [0006](0006-download-speed-block-size.md) | Download speed: 512 KiB blocks, and a progress bar | accepted |
| [0007](0007-decentralising-the-pointer.md) | Decentralising the pointer: Delivery now, a chain later | accepted |
| [0008](0008-automatic-updates.md) | Automatic updates, app details, uninstall | accepted |
- [0009](0009-private-repos.md): private repos — a read key in the link, encrypted index, pointer and files.
- [0011](0011-basecamp-app.md): Backpack for Basecamp — backpack_core + backpack_ui, publisher and member roles (proposed).
