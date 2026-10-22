# 0001. A repo is an ed25519 key; everything it serves lives in Logos Storage

**Status:** accepted (2026-10-22)

## Context

We ship companion apps for Logos (Loam, Scala, Qaku, Kith, …) through self-hosted F-Droid repos.
F-Droid needs a web server that stays up and an HTTP path to every phone. Basecamp is moving its
packages (.lgx) onto Logos Storage; phones should get their apps the same way.

## Decision

- **Every byte a repo serves is a Logos Storage object addressed by CID**: the index, every APK,
  every icon.
- **A repo is identified by an ed25519 public key**, not by a URL or a server.
  - The index is signed with it ([0002](0002-security-model.md)).
  - A phone that knows the key accepts the repo's content from any Storage node that holds it.
- **A repo link**, `backpack://repo?i=<indexCID>&k=<key>&n=<name>&p=<peerId>&a=<multiaddr>&s=<spr>&u=<pointer url>…`,
  carries:
  - the key;
  - a starting index CID;
  - the Storage nodes to dial first (a NAT-ed phone cannot rely on DHT discovery alone);
  - optionally, where to look for newer indexes ([0003](0003-updates-signed-pointer.md)).
- **The desktop side is a CLI** (`cli/backpack.mjs`). It talks to a Storage node's REST API, either
  the VPS node over the mesh or a local one. The phone side is an Android app with a fetch-only
  libstorage node ([0005](0005-phone-libstorage.md)).

## Consequences

- No web server is needed for content. Any Storage node that pins the CIDs can serve them, and a
  hub that caches them makes the repo survive the uploader going away.
- Storage objects are immutable. A new release means a new index CID, and phones have to learn
  about it. That is [0003](0003-updates-signed-pointer.md).
- Logos Storage is not IPFS. CIDs and the wire protocol are its own.
