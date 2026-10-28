# 0013. Member repos: a key bundle instead of a key in the link

**Status:** accepted (2026-10-27); implemented in backpack_core/ui 0.4.0 and Backpack 0.2.17 (verified in Basecamp 0.3.2 +
emulator against the LEZ test chain: join, renew, add and take off a phone, each followed by an automatic publish)

## Context

A private repo (ADR 0009) has one read key, carried in its invite link: whoever has the link can read,
and a link can be forwarded. For a repo with **members** (people who paid, ADR 0010 / backpack-access,
or simply devices the publisher chose) the read key must reach each member's **device** without a
shareable secret, and stop reaching a device once it is no longer a member.

## Decision

A **member repo** is a private repo whose read key changes every **epoch** and is handed to each member
device in a signed **key bundle** on Logos Storage.

- **Device key.** Each phone has an X25519 key pair (Backpack keeps the secret in its storage; later it
  can be derived from the Loam identity). Backpack shows the public half as a QR:
  `backpack://member?pk=<b64url 32 bytes>&n=<device name>`. It is not secret: it only lets someone
  *give* that device access.
- **Members.** The publisher's list of device keys: added by hand (scan the phone's QR in the Basecamp
  app), or the active devices of paid-up LEZ memberships (backpack-access `member_list` + expiry).
- **Epoch key.** A random 32-byte read key per epoch. The index is encrypted with it exactly like a
  private repo's (ADR 0009: AES-256-GCM body, files under their own keys). A new epoch, or a member
  removed, means a new key; files are not re-encrypted.
- **Key bundle.** A signed envelope `{"backpack":1,"kind":"keys","body":…,"sig":…}`, body
  `{"key": <repo key>, "epoch": n, "wrapped": {<device pk b64url>: <blob b64url>}}`. Each blob wraps the
  epoch key for one device: X25519 with a fresh ephemeral key, HKDF-SHA256 (salt empty, info
  `"backpack/keywrap/1"`) to an AES-256-GCM key, nonce 12 random bytes, AAD =
  `repo key (32) ‖ device pk (32) ‖ epoch (u64 LE)`; blob = `ephemeral pk (32) ‖ nonce (12) ‖ ct (48)`.
  The AAD binds a blob to one repo, one device and one epoch: it can't be replayed elsewhere.
- **Pointer.** A member repo's pointer is a normal signed (not encrypted) pointer whose body also names
  the bundle: `{…, "keys": <bundle CID>, "epoch": n}`. It travels on the repo-key topic (ADR 0007): the
  repo's existence isn't secret, anyone may become a member. Since backpack_core 0.4.0 Basecamp sends it
  there itself, so a members repo needs no web address and no CLI step (ADR 0007, 2026-10-28).
- **Link.** `backpack://repo?…&m=1`: no read key. A forwarded link is useless without membership.
- **Phone.** Fetch the pointer → fetch the bundle (verify its signature with the repo key, epoch = the
  pointer's) → find its own pk → unwrap → decrypt the index → as a private repo. No entry: "you're not a
  member (or not yet: the publisher adds new members at its next publish)". The phone keeps the key
  until the pointer names a different bundle CID (not just a different epoch: removing a member re-keys
  within the epoch).

## Consequences

- Removing a member stops new versions from reaching it at the next epoch; what it already downloaded
  stays its own (ADR 0009, "no revocation of the past").
- Each publish where members changed (or an epoch passed) uploads one bundle: ~150 bytes per device.
- The publisher must run: the bundle for a new member appears only at the next publish (a daemon does
  this for LEZ memberships).

## Paid repos (LEZ memberships): the subscriber side

Members can come from a paid membership on LEZ (backpack-access) as well as from the publisher's own list.
Then the person paying, not the publisher, adds the phones:

- **The pointer says how to join.** A paid repo's pointer carries `access: {via: "lez", program, sequencer,
  price, periodMs}`, read from the chain at each publish. The pointer is plain and signed, so someone who isn't
  a member yet can read the terms; the index stays encrypted. A change of terms counts as a change at publish.
- **Basecamp has two sides.** *Following*: repos you follow by their link; for a paid one, Join / Renew with
  your LEZ wallet and account, and your phones by their member codes (add, take off). *My repos*: the
  publisher side, as before. One app, one core: a publisher usually follows other repos too.
- **Only known programs take payments.** The wallet signs what the program asks, so the core pays only
  through programs it knows (a list in `bp_subs.hpp`), whatever the pointer names, and refuses a payment
  if the chain's price differs from the one the user saw.
- **The publisher publishes by itself when someone joins or a device is taken off,** not when a
  membership only runs out: an expired membership's devices just miss the key at the next publish that
  happens anyway, and lose nothing they had. The core reads the chain every few minutes
  (`BACKPACK_WATCH_SEC`, default 300) and publishes only for a joined or removed device. It runs wherever
  the core runs: Basecamp while open, or headless (logosctl) as an always-on publisher.
- **The phone says what to do.** Not a member of a paid repo: "this repo needs a paid membership (price per
  period): join it in Backpack on Basecamp and add this phone's member code there". The phone keeps the
  repo while it waits, so it gets in by itself after the next publish; there's no need to open the link again.
- **Billing period:** set by the membership program (monthly in production; 1 hour on the test chain).
  Per-repo price and period need a config per repo in the program (asked of its author, 2026-10-27).
- **Privacy:** memberships are public on the chain (which account pays for which repo, how many devices).
  Private registration is left for later.

## A new key at every publish (2026-10-28)

The key no longer follows a clock (`epochLenMs`): every publish that ships something makes a new key,
wraps it for the devices that are members at that moment, and names it by the publish's `seq` (the
bundle's `epoch`). A publish with nothing new (same content, same devices) still does nothing.

Why, from the discussion with vpavlin: whoever can decrypt can copy, so no scheme stops a member from
passing on what they already have; a server or DRM wouldn't either. What re-keying does is make sharing
a chore: a key passed on unlocks one release, so keeping a friend up to date means re-sharing at every
release. It also makes leaving free: a member who lapses or is taken off simply isn't given the next key
and keeps what they had, which they would anyway, so expiry needs no publish of its own.

Considered for later: a deposit on LEZ that anyone presenting a phone's private key can claim (the
recipient of a shared key can take the sharer's deposit), and RLN-style anonymous memberships, where a
device proves "I'm a paid member" without saying which and over-use exposes its secret. Both are noted, not
planned; the main gain of RLN would be privacy (today the chain shows which phone keys belong to which repo).

