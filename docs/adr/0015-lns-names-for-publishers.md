# 0015. LNS names for publishers: follow a name, show who publishes

**Status:** proposed (2026-10-28). Not implemented. Builds on LNS, jefdiesel's name registry on the
Logos Execution Zone ([logos-names](https://github.com/jefdiesel/logos-names), registry v5, the `names`
Basecamp module 0.8).

## Context

A Backpack repo is an Ed25519 key (ADR 0001). People reach it through a link that carries the key, the
index CID, the Storage node's address and the update note's sources: correct, but unreadable, and shared
as a QR code or a pasted string. A repo shows its publisher as a key fragment ("publisher key
Tr-OoH…sbNg"), which says nothing to a person deciding whether to install from it, or pay for it
(ADR 0013, 0014).

LNS gives a person a short name (`vpavlin`) with text entries only its current holder can set. As checked
against the registry's source on 2026-10-28:
- every change to a name needs the signer whose owner tag is the record's owner;
- a transfer or a release empties what the last holder stored;
- re-registration after expiry plus grace writes a fresh record;
- `resolve` skips lapsed names.

Names are 1–32 characters of `a-z`, `0-9` and `-`. Since 2026-10-28 Backpack's update note travels over
Logos Delivery under a topic derived from the repo key (ADR 0007), so **a repo key alone is enough to
follow a repo**: the note names the index and the Storage nodes.

## Decision

### 1. A name lists the holder's repos

The name's text entry `backpack.repos` holds the publisher's public repo keys (43-character base64url),
separated by spaces, the main one first. The key follows the LNS advice to namespace application entries.
Only repos whose link carries no secret go there:
- public repos;
- member repos (ADR 0013), whose link has no key either;
- never a private repo's read key (ADR 0009).

A name holds at most 16 entries and 12 KiB, which is room for about 250 repo keys.

### 2. Follow by name

Basecamp's "Follow a repo" and the phone's "Add repo" accept a name as well as a link. Following a name:
1. resolves it and reads `backpack.repos`;
2. offers the repos it lists, or takes the only one;
3. follows the repo by its key, as a link would: the update note comes over Delivery (and from web
   addresses, if a link had any).

Names are prepared exactly as the registry does it: trim, lowercase, strip `lns:`, `@` and a trailing
`.lez`, then check 1–32 of `a-z0-9-` with no leading or trailing `-`. In Basecamp, the module's own
`normalize` does this. On the phone it's a copy with tests against the registry's cases.

### 3. A name finds a repo; the key keeps it

Resolving trusts the one sequencer it asks: LEZ v0.2.5 gives reads no proof, and the testnet endpoint is
plain http in places. A lying endpoint could map `vpavlin` to its own key at first contact, as DNS could
before a key is pinned. So:
- When a repo is added by name, the key is pinned as for a link, and from then on everything is checked
  against that key (ADR 0002). The name is never consulted to decide what to trust.
- Adding by name shows the key's short fingerprint next to the name, so it can be compared with what the
  publisher shows elsewhere.
- If the name later lists a different key, Backpack says so and keeps the pinned key. It never switches.

### 4. "Published by vpavlin ✓", only when both sides agree

A publisher can set their name on a repo. The repo's signed index and update note then carry
`"lns": "vpavlin"`. Basecamp and the phone show the name as the publisher, with a check mark, only when
both hold:
- the signed note or index names the name, which only the repo key can sign;
- the name's `backpack.repos` lists that repo key, which only the name's holder can set.

One side alone shows nothing. This is the two-way rule the LNS spec uses to link a name to a Logos
account (spec section 7): a holder can list any key, and a repo can claim any name. The check runs when
the repo is added and when the note's name or the listing changes, not at every refresh. The badge is a
convenience under decision 3, not a trust anchor.

### 5. Publisher setup in Basecamp, without a terminal

In a repo's Settings, the publisher enters their name. Basecamp then:
1. adds `"lns"` to the next publish;
2. adds the repo key to the name's `backpack.repos` through the `names` module's `set_text`. This needs
   the LEZ wallet that holds the name to be open in the Names app. Otherwise Basecamp shows what to set
   and why it couldn't.

Removing the name, or making a repo private, does both in reverse. A private repo is never listed.

### 6. How it's built

- **Basecamp:** `names` is an *optional* dependency of backpack_core; without it, names simply aren't
  offered. Lookups go one at a time, with progress, and their results are kept for the session. The
  module answers one call at a time, at about 0.4 s per name on a desktop (64 MiB Argon2id plus a
  sequencer read; measured 2026-10-28), so a burst of parallel calls times out.
- **Phone:** the registry's dependency-free resolver (`resolver/names.js`, pure-JS Argon2id) plus a
  sequencer setting, defaulting to the one the `names` module uses. Lookups happen only when a repo is
  added by name and when a publisher badge is checked, never on the update path.

### 7. Now in Backpack, later in loam identity

A name belongs to a person, not to a repo. The general place for it is loam identity (loam_core's
identity service), where every Loam app (Scala, Kith, Qaku, Perun, Backpack) could resolve and show it
once. That has to wait: Backpack's keys aren't loam identities today. So this starts in Backpack, built to
be added to rather than replaced:

- **One seam.** backpack_core looks names up through one small interface (`resolve`, `normalize`,
  `setText`). Today it calls the `names` module. Once loam_core offers names, it asks loam_core instead,
  and nothing above it changes.
- **The claim grows, it isn't replaced.** Today the repo's signed note carries `"lns": "<name>"`, checked
  against the name's `backpack.repos` (decision 4). Later the note also carries `"publisher": <loam
  identity>`, with that identity's signed endorsement of the repo key. The name then links to the identity
  (LNS spec section 7: `logos.account` one way, the identity's endorsement the other), and the identity
  vouches for the repo. The badge accepts either path, so repos published before the change keep their
  badge.
- **The repo list stays where it is.** `backpack.repos` remains how "follow vpavlin" finds the repos.
  Whether the name links to the repo directly or through an identity only changes who vouches for it.
- **Trust doesn't move.** Through either path, the name only finds a repo; the pinned repo key stays the
  authority (decision 3).

The loam identity part gets its own ADR in loam-basecamp when it's taken up.

## Consequences

- Repos become something you can say out loud: "follow vpavlin".
- Paid repos (ADR 0014) show who you are paying, once both sides agree.
- Backpack depends, optionally, on a third-party registry on a testnet. Every flow keeps working without
  it: links, keys, and Delivery are unchanged.
- **Privacy:** anyone who knows or guesses a name can resolve it and see the holder's repo keys. That is
  the point for a publisher, but a person who wants a repo to stay unlisted just doesn't add it.
- First contact by name is only as trustworthy as the sequencer endpoint (decision 3). Pinning, the
  fingerprint and the no-switch rule bound the damage to that first moment.
- Not decided here: the update note's location in the name (to get around Store's one-day window, ADR 0014),
  because every publish would then cost a LEZ transaction; and anonymous "anyone with a name" gates, which
  aren't worth it while testnet names cost next to nothing.

## Order of work

1. Basecamp: the names seam (decision 7), Follow by name (decision 2), and the publisher setup
   (decision 5).
2. The "published by" badge (decision 4), in Basecamp and on the phone.
3. Phone: Add repo by name, after the 0.4.0 / 0.2.17 release candidate has been tested on a real device.
4. Later: loam identity names (decision 7), as an ADR in loam-basecamp first.
