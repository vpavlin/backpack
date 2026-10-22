# 0005. The phone's libstorage: fresh build, PR #1540 download fix

**Status:** accepted (2026-10-22)

## What happened

**1. Scala's prebuilt arm64 `libstorage.so` crashed.**
- Built 2026-09-28 from vpavlin/nim-codex 6af2049, it crashed inside `storage_fetch` (SIGSEGV).
- The crash reproduced on the phone, and under arm64 translation on an x86_64 emulator.
- Rebuilding the same source fixed it.

**2. Large downloads stalled with that commit.**
- Small objects (one 64 KiB block: the index, icons) arrived, but multi-block APKs never did: the
  manifest arrived and the blocks didn't.
- This is the download-swarm bug fixed upstream by logos-storage-nim PR #1540: the swarm ignored a
  holder that was answering. The VPS node already runs that fix.

**3. JNI glue fixes.**
- A timed-out call freed its response while libstorage still held it (use-after-free); now the
  late callback frees it.
- Every libstorage call runs on one long-lived 16 MB-stack thread, because each entry point
  registers its calling thread with Nim's GC.
- Diagnostics run on a second thread.

## Decision

- **Source:** the phone library is built from logos-storage-nim d82d0d39 (PR #1540), plus
  `libStorageAndroid` (cherry-picked a2a353e), plus `patches/nim-libp2p/0001-tcp-dial-bind-same-family`.
- **JNI changes for that API:**
  - `fetch(ctx, cid, isPrivate=false, advertise=false)`;
  - `download_init(…, local, isPrivate=false, advertise=false)`;
  - `download_stream` without `local`.
- **Builds:** arm64-v8a for phones; x86_64 for emulator tests only (`BACKPACK_EMU=1`), which run the
  library natively instead of under translation.
- **Data dir:** the Storage data dir is a cache, wiped at every launch (a dir left by a killed
  download made the next one hang).
- **Provenance:** `native/logosstorage/PROVENANCE` records the source, patches and hashes.

## Consequences

Scala's phone app ships the old prebuilt library and has the same latent crash and stall. The
same rebuild applies there.
