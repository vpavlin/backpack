# 0007. Decentralising the pointer: Delivery now, a chain later

**Status:** accepted, implemented (2026-10-23)

## Context

Everything a repo serves is in Logos Storage except one thing: the signed pointer to the newest index
(ADR 0003). The pointer is served from HTTPS URLs today (data.vpavlin.xyz). It is safe there, because
the phone checks the repo key's signature and refuses an older `seq`. But it is not decentralised:
if the URL goes away, phones stop seeing updates.

The pointer is about 300 bytes, signed, and carries its own order (`seq`). So any transport that can
deliver "the newest message for this key" works, and the transports can be combined: the phone takes
the highest valid `seq` from all of them.

## Options

| | Logos Delivery | LEZ (execution zone program) | Logos blockchain directly |
|---|---|---|---|
| On the phone | Yes: our apps already run a Delivery node (Loam shared node) | Needs an RPC endpoint or an indexer; no light client on phones | Needs a full or light node; not possible on a phone today |
| Durability | Store nodes keep messages for a limited time; the publisher must re-send | Permanent, in chain state | Permanent |
| Ordering | None; `seq` in the signed pointer gives it | Chain order | Chain order |
| Cost to publish | Free (RLN rate limit) | A transaction | A transaction |
| What it adds | Removes the web dependency | A public record: who owns which key, key rotation, revocation | Same as LEZ |

## Decision (proposed)

1. **Delivery is the transport.** The publisher sends the signed pointer to the content topic
   `/backpack/1/<repo key>/json`. Phones:
   - subscribe while open, so a publish arrives within seconds;
   - on start, query Store for the topic and take the highest valid `seq`;
   - go through the Loam shared node (loam-transport client), so Backpack doesn't run its own
     Delivery node.
2. **Keep it alive.** Store keeps messages only for a limited time, so the latest pointer is re-sent
   periodically (daily) by the publisher's always-on box. A mirror (ADR 0006: desktops and always-on
   boxes holding a repo's CIDs) can re-send it too, without the key: re-sending a signed message
   needs no signature of its own.
3. **HTTP stays as one more source**, not the only one. A phone uses every source it knows and the
   highest `seq` wins.
4. **A chain later, for keys rather than updates.** What Delivery can't give is a durable, ordered
   record of a repo's key: rotation after a lost key, revocation after a stolen one, a human name
   bound to a key. That fits an LEZ program well. Updates themselves don't need a chain: `seq` and
   the signature already order and authenticate them. Revisit when phones can read LEZ state
   without trusting a single RPC endpoint.

## Consequences

- Backpack takes on loam-transport as a dependency, and the Loam app on the phone.
- RLN: one pointer per publish plus one re-send a day per repo is far below any tier (see the RLN
  traffic study).
- The `u=` link parameter stays for HTTP sources; the Delivery topic is derived from `k=` (for a private
  repo, ADR 0009, from the read key `x=`; the phone listens on both), so links
  need no change.

## Implementation (2026-10-23)

- **Publish:** `ops/pointer-announce/send.sh` on the VPS sends the pointer file through the scala hub's
  `delivery_module.send` (logos.test, cluster 2, same network as the phones). The CLI runs it after
  every `publish` (an `exec:` pointer target); a systemd timer re-sends all pointers every 12 hours.
- **Verified:** a fleet Store query (us-central and hongkong store nodes) returned the message, and it
  opened as a valid pointer with the repo key (seq 4).
- **Phone (0.2.3):** `mobile/src/delivery.ts`, loam-transport as a client of the Loam app's shared node
  (`preferServiceBackend` before `start`). It listens on every repo's topic and pulls Store on start; a
  pointer that opens with the repo key and has a higher `seq` triggers a refresh and then automatic
  updates. Without Loam, start fails quietly and the HTTPS sources carry on.
- **Not testable on the emulator:** the Loam app's node needs arm64 (`librln.so`), so the receive path is
  verified on a phone only.
- Gotcha: `logosctl call … b64:<…>` does NOT decode base64 for a `bstr` argument; it sends the literal
  text. Pass the JSON with `str:`.
- **Pull-to-refresh asks Store** (0.2.6): it triggers the Loam service's Store query for Backpack's
  topics, waits up to 8 s for every repo's note to come back through the receive callback, then decides.
  With the web switch off (the default since 0.2.5), no HTTP request is made at all.

## Basecamp sends it itself (2026-10-28, backpack_core 0.4.0)

- **Publish:** every publish sends `latest.json` (the signed note, verbatim) to `/backpack/1/<id>/json`
  through loam_core's shared node, on plain relay (`setTopicMode("/backpack/", "relay")`), not SDS
  channels. While Basecamp runs it sends every published note again: once ~8 s after start, then every
  12 hours. No web host, ssh or CLI step is needed; `pointers.put` targets still run when set.
- **Store window:** the logos.test store nodes refuse a query range over 24 h ("time range exceeds 24h"),
  so a note is findable for about a day after it was last sent. The 12-hour resend keeps it there while
  the publisher's Basecamp is open; the VPS timer (above) covers a publisher that is often off, if the
  note is also rsynced there.
- **Following in Basecamp:** a followed repo's note comes from Store through `delivery_module.storeQuery`
  (pubsub topic by autosharding: cluster 2, 8 shards; `/backpack/` lands on shard 1), asked of all six
  fleet nodes at once, then from any `u=` web addresses; the highest valid `seq` wins. On 2026-10-28 the
  three node-01 peers refused the dial and node-02 (Hong Kong) answered: asking every node matters.
- **Wire:** the payload as Store returns it is base64 of base64 of the JSON (loam_core's bearer layer);
  the phone's `payloadCandidates` tries one and two decodings, and the desktop reader does the same.
- **Verified (logosctl, two sessions, no web address, no pointer target):** publish of a members repo →
  "Message successfully sent" on the fleet; publisher stopped; a second session followed the link and got
  note #1 from Store; the restarted publisher re-sent it 8 s after loading. Not yet verified: a real phone
  receiving it through Loam (the emulator can't run Loam).

