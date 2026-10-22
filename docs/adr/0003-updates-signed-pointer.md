# 0003. Updates: a signed pointer to the newest index, served from anywhere

**Status:** accepted (2026-10-22)

## Context

A Storage object cannot change, so a new APK means a new index with a new CID. Phones hold an old
CID. They need a way to learn the new one that does not reintroduce a trusted server.

## Decision

- **The signed pointer.** `publish` writes a small signed **pointer**, `latest.json`:
  `{"backpack":1,"kind":"pointer","body":"{\"key\",\"seq\",\"index\":<CID>,\"created\",\"nodes\"}","sig":…}`.
  It is signed by the repo key exactly like the index ([0002](0002-security-model.md)).
- **The pointer carries no trust of its own; only the signature and `seq` matter.** So it can be
  served by anything:
  - a plain HTTPS file: data.vpavlin.xyz today, any mirror tomorrow;
  - the repo link (`i=`, the CID at the time the link was made);
  - later, a Logos Delivery content topic derived from the key (`/backpack/1/<key>/json`). There
    the CLI publishes the pointer and the store keeps the latest for phones that were offline.
- **How the phone updates.** On start and on pull-to-refresh, for each repo, the phone:
  1. fetches every pointer URL it knows: from the link (`u=`, repeatable) and from the last
     accepted index's `pointers`;
  2. keeps the valid pointer with the highest `seq` above the stored one;
  3. fetches that index CID from Storage and checks that the index itself says the same `seq`;
  4. replaces the stored index.

  Opening any newer `backpack://` link for the same key does the same. Rollback is refused either
  way.
- **Each publish increments `seq`.** It is stored in `~/.backpack/<repo>/repo.json`.

## Consequences

- **HTTPS is used only for liveness, never for integrity.** A malicious mirror can hold back
  updates (freeze) but cannot forge or roll back. Several pointer URLs make freezing harder.
- **Pointers must be readable without special trust.** Android apps don't trust user-installed
  CAs, so a mesh-only HTTPS host with our own CA can't be the default pointer host. A public one
  can.
- **Delivery comes next.** Making the pointer a Delivery message removes the last web dependency,
  but Backpack would then need a delivery node, via the shared Loam node. That is a separate step.
