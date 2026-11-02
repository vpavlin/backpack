# ADR 0016: Backpack offers to install Loam

Status: accepted (2026-11-02, vpavlin), Backpack 0.2.20

## Context

Backpack learns about new repo versions from signed update notes. They arrive over Logos Delivery, which Backpack gets only through the Loam app, or from a repo's web address, which is off by default (Settings, ADR 0007). Without Loam and with default settings, Backpack therefore never hears about any update.

Dropping Backpack's own Storage library (about 9 MB of the 32 MB APK) was considered. It was kept: without it, Backpack couldn't fetch anything, Loam included, before Loam is installed.

## Decision

- **First run:** on a phone without Loam (`xyz.vpavlin.loam`), Backpack asks once: "Install Loam too?", with why.
- **While Loam is missing:** Settings shows an "Install Loam" card.
- **Where Loam comes from:**
  1. The newest Loam that any of the user's repos lists.
  2. Otherwise, a built-in source: vpavlin's public repo as of the build. Its link is in `src/loam-setup.ts`; it carries no read key. Its list is fetched with Backpack's own Storage.
- **Which Loam is offered:** only versions signed by Loam's pinned key, `LOAM_SIGNER`. Install then checks it like any app: sha256 from the signed index, signer, package, versionCode.
- **Fallback:** if the built-in list can't be fetched (for example its CID is gone), Backpack offers, this once, to ask that repo's web address for its newest signed note. Only the note comes from the web; Loam still comes from Storage.

## Consequences

- The built-in list is the one current at build time: the Loam that Backpack was released with. A newer Loam arrives as a normal update once Loam runs.
- The built-in link's index CID must stay fetchable. The VPS node keeps blocks for 100 years since 2026-11-02, and followers mirror the repo (ADR 0015).
- A new Loam signing key means a Backpack release, since `LOAM_SIGNER` is pinned.
