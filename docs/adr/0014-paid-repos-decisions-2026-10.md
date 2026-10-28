# 0014. Paid repos: what we decided, and what we left for later (October 2026)

**Status:** accepted (2026-10-28, with vpavlin). A proof of concept: "anything that works is a good demo",
to be iterated. Details live in ADR 0007 (Delivery), 0011 (Basecamp app) and 0013 (member repos).

## Decisions

1. **One Basecamp app, two sections.** "Following" (the default: repos you read or pay for) and
   "My repos" (repos you publish). The user joins and pays in Basecamp, then adds their phone; the phone
   never asks the user to show a code to the publisher. (2026-10-27)
2. **Monthly memberships, set by the publisher.** Price and period (month by default; week, day or hour)
   are chosen in Basecamp and written to the repo's own config on LEZ (one config per repo, keyed by the
   repo key, in the multi-publisher `backpack_access` program). (2026-10-27)
3. **Joins publish, expiry doesn't.** A device joining, or taken off, makes the publisher's Basecamp publish
   by itself. A membership that only runs out triggers nothing: the lapsed member is left out of the next
   publish. (2026-10-27; verified: 6 h past an expiry, no publish)
4. **No terminal.** Everything a publisher or member does happens in Basecamp. In particular the update
   note goes out over Logos Delivery from Basecamp itself (loam_core), so a repo needs no web host, ssh or
   CLI step; web addresses are an optional extra. A follower's Basecamp reads the note from Store.
   (2026-10-28)
5. **We don't try to stop sharing.** Whoever can decrypt can copy: no server, DRM or zero-knowledge proof
   prevents a member from passing on what they already have. Membership is trust plus convenience: members
   get each release as it ships. (2026-10-28)
6. **A new key at every publish.** Each publish that ships something re-keys, for the devices that are
   members at that moment. A shared key opens one release, so keeping a friend up to date means re-sharing
   every time; and leaving costs nothing to enforce: a lapsed or removed member simply doesn't get the next
   key, and keeps what they had (which they would anyway). (2026-10-28)

## Known limits we accept for now

- **Store keeps a note for about a day** (the logos.test store nodes refuse a range over 24 h). Basecamp
  sends every note again every 12 h while it runs; if the publisher's Basecamp is closed for longer, a new
  phone or follower finds nothing until it is opened again (or a web address is set).
- **Joining needs the publisher online.** Only the repo key can let a new member in; while the publisher's
  Basecamp is closed, a member who paid waits for it.
- **The chain shows which phone keys belong to which repo.**

## Considered, not now

- **Hubs.** A *keeper hub* (no keys) that re-sends everyone's signed notes so Store keeps them; a
  *publisher hub* (backpack_core headless, holding the key) as the single publisher of a repo, with
  Basecamp as its remote control. Not built: a keeper doesn't solve joining, and a publisher hub needs a
  single-writer handover.
- **Members let members in.** Any key is self-verifying (it either opens the publisher-signed index or it
  doesn't), so any member's Basecamp could wrap the current key for a new paid device it sees on chain.
  Fits decision 5; would remove "joining needs the publisher online".
- **The note on chain.** The publisher writes the latest note (seq, index CID, key bundle CID) into the
  repo's LEZ config; Basecamp and phones (through the sequencer's HTTP API; the note is signed, so an
  untrusted endpoint can only serve a stale one) read it there. Would remove the one-day Store limit.
- **A deposit anyone can claim with your phone's key.** Joining stakes a deposit; presenting a member
  phone's private key to the program claims it. Whoever you share access with can take your deposit.
  Covers the reusable secret only, not one release passed on by hand.
- **RLN-style memberships.** A device proves "I'm a paid member" without saying which, and over-use
  (more fetches per release than devices allowed) exposes its secret for slashing. Main gain: privacy.
  Costs: proofs on phones, a membership tree that drops expired members, and someone online to answer.
- **Per-member watermarks** in the APK Signing Block: possible, trivially stripped, and N copies of every
  APK. Rejected.
