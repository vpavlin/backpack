# 0009. Private repos: a read key in the link

**Status:** accepted, implemented (2026-10-27): CLI 5405fa2, app 0.2.13; emulator-verified with the lab repo. Audited 2026-10-27 (proteus/auditor, PR-1..PR-13), fixes in 0.2.14.

## Context

A repo is public by construction (ADR 0001). The index, APKs and icons are Storage objects fetched by CID,
and the pointer that names the index CID is public: it goes over a Delivery topic named after the repo key
and is served over HTTPS. So anyone who has the link, or who watches the topic, can list a repo and download
everything in it. vpavlin's "dev repo" mirrors a LAN F-Droid repo with apps that were never meant to be
public.

Signing does not help here. It proves who wrote the index; it hides nothing.

The obvious first idea is to encrypt the APKs and put their keys in the index. On its own that hides
nothing either: the index is a public CID, so anyone can read the keys next to the CIDs. A key has to
reach readers by some other way than the repo's public objects.

## Decision

A repo can be **private**. A private repo has one secret **read key** (32 random bytes) besides its
signing key. Whoever holds the read key can read the repo; nobody else can.

1. **The read key travels in the link** as `x=<b64url>`. The `backpack://` link becomes a capability:
   share the link, share the repo. The phone keeps the read key with the repo.
2. **The index is encrypted with the read key, then signed.** The envelope's `body` is
   `b64url(nonce ‖ AES-256-GCM(readKey, nonce, json))` and `enc` says so. `sig` is ed25519 over the
   `body` string, as for every envelope, so the signature is checked before anything is decrypted. The
   decrypted JSON is the same index as today (it still names the repo key; ADR 0002's checks are
   unchanged).
3. **Each APK and icon has its own random file key**, listed only in the encrypted index next to its
   CID (`fk`), with the ciphertext size (`csize`). The read key never encrypts a file. So:
   - rotating the read key re-encrypts the index only, not hundreds of megabytes of APKs;
   - one app can later be shared on its own by handing out its file key.
4. **Files are encrypted in chunks** (format `bpe1`), so the phone decrypts while copying, with flat
   memory, in native code (Kotlin `Cipher`, not JavaScript):
   - header: `"BPE1"` ‖ chunk size (u32 BE, 1 MiB) ‖ 8-byte random nonce prefix;
   - each chunk: AES-256-GCM(fileKey, nonce = prefix ‖ u32 BE index, aad = 1 byte: 1 on the last chunk,
     else 0), ciphertext ‖ 16-byte tag;
   - the last-chunk flag catches truncation; reordering fails the tag.
   The plaintext checks don't change: SHA-256 from the index, the pinned signing certificate, the package
   and versionCode, the installed app's signer (ADR 0002).
5. **The pointer is encrypted the same way**, so the index CID and `seq` are hidden too. The envelope
   keeps the repo key in the clear (`key`) so a relay (the VPS `send.py`, a mirror) can check the
   signature without the read key.
6. **The topic and the web file are named after the read key**, not the repo key:
   `t = b64url(SHA-256("backpack/topic/1" ‖ readKey))`, topic `/backpack/1/<t>/json`, file `<t>.json`.
   Someone who only knows the repo key can't find the private repo's traffic.
7. **Loam sees only ciphertext.** Its Storage cache (loam-transport ADR 0020) fetches CIDs; Backpack
   decrypts after copying the file out of Loam's cache.
8. **A whole repo is private or public**, not single apps. One flag (`private`) on the repo. Mixing them
   would put public and private CIDs side by side in one index and gain nothing.

### Publisher (CLI)

- `backpack init --private` makes a private repo; `backpack config --private` turns an existing one
  private (new read key, every file re-uploaded encrypted on the next publish). The read key is stored in
  `repo.json` (mode 600) and printed in the link; it is as secret as the signing key's *read* half.
- `backpack config --rotate-read-key` makes a new read key. Readers need the new link. Old content stays
  readable to whoever had the old key (see below).
- `publish` uploads every not-yet-encrypted file, writes the encrypted index and pointer, and prints the
  link with `x=`.

### Phone (app)

- `parseLink` reads `x`; adding a repo, or opening a newer link for a known repo, stores the read key.
- Delivery listens on the read-key topic; the web pointer is fetched from `<t>.json`.
- Downloads go to `<path>.bpe`, then the native module decrypts to `<path>` and deletes the ciphertext;
  then the same checks as before.
- An older Backpack given a private link fails to read the encrypted index (a JSON parse error), and keeps
  showing nothing new.
- A link's read key is a candidate until it decrypts the repo's index; only then is it stored. The phone
  listens on the read key's topic AND the repo key's, and tries every repo on a shared topic (audit PR-1, PR-3).

## What this doesn't do

- **No revocation of the past.** Rotating the read key stops a removed reader from seeing new versions.
  Everything they could already read stays theirs: the old files are still on Storage, and their keys were
  in an index they decrypted. True of any shared-key scheme.
- **Metadata.** Sizes of the ciphertexts, publish times, and (for relay operators) which repo key
  publishes when are visible.
- **The link is the secret.** Anyone who sees it can read the repo, and it can be forwarded. Treat it like
  a password; the QR the CLI prints is the same secret, and so is `lastLink` in repo.json and the CLI's
  output (keep it out of CI logs).
- **Converting a public repo keeps its past public.** The old plaintext indexes, APKs and the old pointer
  file stay on Storage and the web: whoever had the old link can still read everything published before
  the switch. Only what is published after it is private.
- **No forward secrecy.** One read key opens every index it encrypted; a key that leaks later opens the
  repo's whole past under that key. Rotating limits the damage to what was published under the old key.
- **The repo key is visible.** Encrypted pointers carry the repo key in the clear (for relays), so anyone
  watching a topic learns which repo publishes on it, and successive read keys of one repo are linkable.
- **Keys at rest.** The read key is in repo.json (mode 600) on the publisher's box and in the app's storage
  on phones; neither is encrypted at rest.
- **A plaintext envelope is still accepted** when it verifies (that's how a public repo works). A private
  repo stays private only while it is published with this CLI, which always encrypts once `--private` is set.
- **Files at the 512 MiB edge.** Encryption adds 16 bytes per MiB; a file that no longer fits Loam's
  per-file cap is fetched with Backpack's own Storage node instead (audit PR-7).

## Later: membership instead of a shared link

Instead of one key in a link, each reader has a key pair (a Loam per-app identity fits), and the index
header carries the read key wrapped for each member. Then a reader can be removed cleanly and the publisher
knows who has access. The file format above doesn't change: only how the read key reaches a phone.

vpavlin would like that membership to live in a LEZ program: people register themselves, maybe pay a
subscription, and get access. A program can't keep a secret, so the program would hold the membership
(public keys, expiry) and something off chain would wrap the current read key for each paid-up member.
That design is being looked at with the LEZ/SPEL team (laptop/SPEL) and will get its own ADR.
