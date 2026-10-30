# 0015 Followers mirror the repos they follow

Status: accepted (2026-10-30)

## Context

A repo's files live on Logos Storage, on the node its publisher chose (for our repos, a VPS node,
because our desktops are behind NAT). That node is the only place they come from. If it is down or
slow, nobody can install.

Storage is content-addressed. A phone starts its node with the repo's nodes as DHT bootstrap, dials
them, then `fetch(cid)`: blocks can come from any provider the DHT knows, not only the listed nodes.
Every file is checked: by its CID, by the signed index that names it, and APKs by their pinned signer.
So a copy held by anyone is as good as the publisher's, and more copies mean more places to fetch
blocks from.

People who follow a repo in Basecamp already run a Storage node (Basecamp owns one). Hubs and
Basecamps running a Logos blockchain node are online most of the time.

## Decision

1. **Basecamp mirrors the repos it follows.** For each followed repo `backpack_core` keeps the files of
   its latest signed update in the Basecamp's own Storage node, with `fetch(cid, advertise=true)`: the
   node announces them in the DHT and serves them. On by default, with a per-repo switch.
2. **The publisher says what to mirror.** A follower can't always read the index: a private repo's is
   sealed with the read key, a members repo's with a key only member phones get. So each publish
   uploads a **mirror list**, a public JSON file of CIDs (`{"v":1,"key":…,"seq":…,"cids":[…]}`: the
   index, the members key bundle, icons and every APK version the index lists), and the signed pointer
   names it as `mirror`. The pointer stays small (it travels as a Delivery note and is fetched with an
   8 KB cap). A follower trusts the list because the signed pointer names its CID. Phones ignore the
   field (they keep only the pointer fields they know).
3. **What a mirror reveals.** The list shows how many files a repo has and their CIDs. A CID gives the
   size and the stored bytes. For private and members repos those bytes are encrypted (ADR 0009, 0013),
   so a mirror serves ciphertext it cannot read, and anyone can mirror a paid repo without being a
   member. The names of apps stay in the sealed index.
4. **Cleanup.** The mirror holds exactly what the latest list names. A file that leaves the list (a
   version the publisher dropped) and every file of an unfollowed repo is removed with `remove(cid)`.
   Only files the mirror fetched are ever removed (state in `<data dir>/mirror.json`), never other
   data in the shared node (Scala's attachments, Dufs).
5. **Honest status.** The repo shows what is held for others and whether the node announces any
   address at all. A node behind NAT without relays holds copies but serves nobody, and says so.
6. **Storage version.** `backpack_core` depends on `storage_module`. Basecamp prefers an installed
   storage_module over its bundled one (verified 2026-10-30), so our 3.0.3 (3.0.2 + libstorage
   PR #1540, the download-swarm fix) in our repos replaces the bundled 3.0.2 until upstream releases
   the fix.

## Consequences

- Repos keep working while the publisher's node is down, as long as one reachable follower holds the
  files. Headless hubs following each other's repos are the next step (the same module under
  logosctl).
- Followers spend disk and upload. The per-repo switch turns it off, and the repo shows the size.
- Old publishers' pointers have no `mirror`: their followers hold the index (and key bundle) only,
  until the publisher publishes with this version.

## Results (2026-10-30, crib, Basecamp 0.3.2 with storage_module 3.0.3)

- A follower holds a repo's files within seconds of its update (index, key bundle, list, APK); Stop
  hosting removes them, Host again refetches them.
- Desktop to desktop through the DHT works: with every file deleted from the publisher's node, a second
  Basecamp that followed the repo fetched all of them (incl. a 639 kB APK) from the first follower in
  about a minute.
- The phone (emulator, Backpack through Loam 0.0.63's Storage) did not get the index from the mirrors
  in 10+ minutes. Not explained yet; to test on a real arm64 phone and debug Loam's fetch path.

## Open

- Storage expires blocks after `block-ttl` (30 days by default). Whether a repeated `fetch` of a held
  CID keeps it alive is not known yet: test, then refresh accordingly.
- Do phones actually use a follower's copy? Test: a repo whose publisher node is down, installed on a
  phone while only a follower holds it. If DHT discovery is not enough, followers announce themselves
  (over Delivery) and phones dial them like the listed nodes.
