# 0012. Keycard for Backpack's keys

**Status:** proposed, not scheduled (2026-10-27). Recorded so the direction is known; no implementation yet.

## Context

Backpack holds two kinds of keys on the publisher's computer (`~/.backpack`):

- the **repo key** (ed25519): signs the index and the pointer, and is what every phone trusts for that repo;
- **APK signing keys** (RSA-4096 by default, one per app, ADR 0011): the app's identity on Android; every
  later version must be signed with the same key.

Both are files today (mode 600). A stolen or compromised computer can publish to the repo and sign app
updates. A hardware key would make every publish or signature need the holder's physical approval.

**Keycard** (the NFC card and Keycard Shell) only does **secp256k1**. That limits what it can do here:

- **It can't sign APKs.** Android's APK signature schemes accept RSA, DSA and ECDSA on the NIST curves
  (P-256 and up); a secp256k1 signature is rejected at install.
- **It can't hold today's repo key**, which is ed25519.
- Keycard Shell gained OpenPGP support (keycard-tech/keycard-shell#227, merged 2026-10-14): a
  secp256k1 OpenPGP v4 identity derived at a firmware-fixed path, "create identity" and "sign message"
  (up to 255 bytes, reviewed on the device's screen), over QR (`UR:BYTES`). It is not an OpenPGP card
  applet (no `gpg --card-status`), and the curve is still secp256k1, so it doesn't change the APK point.

## Decision (proposed)

Use Keycard in the two ways it fits, repo key first:

1. **The repo key on Keycard.** Add a secp256k1 repo-key type next to ed25519 (envelope `alg` field;
   phones verify with `@noble/curves`, already a dependency; old repos unchanged). Publishing then needs
   the card: an NFC/PC-SC tap (the Loam keycard code: loam-keycard, ADR 0001 there), or a Keycard Shell
   round trip by QR, where the Shell shows what is being signed (the index hash, seq and repo name) and
   signs after approval. Signing is over a 32-byte hash, so Shell's 255-byte message limit is no issue.
   This protects the key that matters most: the one every phone trusts.
2. **APK keys locked by Keycard.** The APK keys stay ordinary RSA keys (Android requires that), stored
   encrypted. The encryption key is derived from a deterministic (RFC 6979) Keycard signature over a
   fixed, Backpack-specific challenge, so unlocking a key needs a tap. The encrypted key file still needs
   its backup, and the card (or its seed) is needed to open it.

Hardware signing of APKs themselves, if ever wanted, goes through a PKCS#11 device that holds RSA or P-256
keys (YubiKey PIV, an OpenPGP card): `apksigner` supports PKCS#11, and Backpack could too.

## Open

- The envelope change for secp256k1 repo keys (field name, low-S rule, test vectors for CLI, phone, C++).
- How an existing ed25519 repo would move to a Keycard key (a signed hand-over note in the old key's
  last pointer?) or whether only new repos get it.
- Recovery when the card is lost: the Keycard seed restores the same derived key; document the path.
