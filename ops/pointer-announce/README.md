# Pointer announce (ADR 0007)

Runs on the box that already receives the pointer files (`publish` rsyncs them to
`/srv/data/{public,unlisted}/backpack/<key>.json`) and has a Logos Delivery node (here: the scala hub,
a logosctl session with `delivery_module`, logos.test cluster 2).

- `send.sh [KEY…]` sends each pointer file verbatim to `/backpack/1/<key>/json`. No key material: the
  pointer is already signed.
- `backpack-pointer.timer` re-sends every pointer twice a day, so fleet Store keeps them.
- The CLI announces right after publishing:
  `backpack config --pointer-put 'exec:ssh root@<box> /root/backpack-pointer/send.sh "$BACKPACK_KEY"'`

Install: copy `send.sh` to `/root/backpack-pointer/`, the units to `/etc/systemd/system/`, then
`systemctl daemon-reload && systemctl enable --now backpack-pointer.timer`. Adjust `CTL` to your
logosctl session.

Check what Store holds: `delivery_module storeQuery` against a logos.test store node (see ADR 0007).
