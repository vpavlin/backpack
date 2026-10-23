# 0006. Download speed: 512 KiB blocks, and a progress bar

**Status:** accepted (2026-10-23)

## The problem

APKs arrived at 60-130 KB/s from the repo node on the VPS, whether on a phone, the emulator or a
desktop. A 39 MB app took 5-6 minutes. With no progress shown, a download looked like a hang.

## What it is not

All measured on 2026-10-23 with libstorage d82d0d39 (PR #1540), the same build as the VPS node:

| Test | Result |
|---|---|
| Same file, plain HTTP from the same VPS node's REST API | 10.6 MB/s |
| 8 MB over HTTPS from the VPS public IP | 12 MB/s |
| VPS node CPU during a p2p download | ~0% |
| Two nodes on one machine, no network in between | 0.6-1.7 MB/s |
| The same two nodes with 26 ms round trip added on localhost (netem) | 254 KB/s |
| Crib to VPS (27 ms round trip), 64 KiB blocks | 58-120 KB/s |
| Crib to VPS, 512 KiB blocks | 200 KB/s (3.4x) |

- Not the server's bandwidth or CPU: HTTP from the same node is ~100x faster.
- Not a relay: the phone dials the node's public address directly.
- Not where the data was uploaded: the node holds every block (`/data/<cid>/exists`).
- It is Storage's block exchange: throughput falls with round-trip time even between two local nodes
  with no packet loss, so each block costs about a round trip or more. A bigger block carries more
  data per round trip.

## Decision

- **Upload files over 1 MiB with 512 KiB blocks**, the maximum Storage accepts
  (`POST /data?blockSize=524288`). The index and icons keep 64 KiB. Each version records its
  `blockSize`.
- **`backpack rehost`** re-uploads existing versions with the new block size, after checking the
  node's copy against the pinned SHA-256. A `publish` then makes the new CIDs live. Phones need no
  update: they read the block size from the manifest.
- **The app shows a progress bar** with bytes, percent, speed and time left. Progress is measured as
  the larger of the bytes landing in the node's store and the bytes written to the file.
  `storage_fetch` returns once the manifest is in, and most blocks arrive during `download_stream`.
- **The app asks for the install permission after the download**, not before; the checked file
  stays cached for the second tap.

Measured on the emulator after the change: Kith (39 MB) in ~2.5 minutes at 220-335 KB/s, down from
~5.5 minutes.

## More nodes serving the same CID

Three seeders vs one, all on one machine with 26 ms round trip (netem), 16 MB random file, three
runs each, alternating order:

| Blocks | 1 seeder | 3 seeders |
|---|---|---|
| 64 KiB | 1.1 MB/s, 423 KB/s, 423 KB/s | 287 KB/s, 158 KB/s, 472 KB/s |
| 512 KiB | 129 KB/s, 173 KB/s, 256 KB/s | 454 KB/s, **6.0 MB/s, 6.3 MB/s** |

- With 512 KiB blocks, extra seeders help far more than linearly: two of three runs reached the
  bandwidth of the machine. Each peer is a pipeline with a small depth, so more peers mean more
  requests in flight.
- With 64 KiB blocks, more seeders did not help.
- Runs vary a lot (one case ranged from 129 KB/s to 1.1 MB/s), so this is a direction, not a number.
  Real seeders have different round-trip times.

So mirrors (people's nodes holding a repo's CIDs) are worth building, together with large blocks.

## Still open

- Upstream (logos-storage-nim, checked 2026-10-23): no open issue or PR on throughput against round-trip
  time. Nearest: #974 (block exchange performance, closed), #895 (baseline vs BitTorrent, closed),
  #1556 (provider lookup discards providers, open), PR #1540 (swarms from DHT providers, open).
  Repro: local nodes and `tc qdisc add dev lo root netem delay 13ms`.
- An HTTP mirror as a fallback would be safe (every APK is checked against the signed SHA-256 and
  pinned signer), but would step around Storage. Not done.
