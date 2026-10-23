# Pointer announce (ADR 0007)

Runs on the box that already receives the pointer files (`publish` rsyncs them to
`/srv/data/{public,unlisted}/backpack/<key>.json`) and has a Logos Delivery node (here: the scala hub,
a logosctl session with `delivery_module`, logos.test cluster 2).

- `send.py [KEY…]` sends each pointer file verbatim to `/backpack/1/<key>/json`. No key material: the
  pointer is already signed.
- `backpack-pointer.timer` re-sends every pointer twice a day, so fleet Store keeps them.
- The CLI announces right after publishing:
  `backpack config --pointer-put 'exec:ssh root@<box> /root/backpack-pointer/send.py "$BACKPACK_KEY"'`

Install: copy `send.py` to `/root/backpack-pointer/`, the units to `/etc/systemd/system/`, then
`systemctl daemon-reload && systemctl enable --now backpack-pointer.timer`. Adjust `CTL` to your
logosctl session.

Check what Store holds: `delivery_module storeQuery` against a logos.test store node (see ADR 0007).

**Hardening (audit BP-2, 2026-10-23).** It runs as root, so it trusts nothing in the directory: only regular
files (no symlinks, opened with `O_NOFOLLOW`), only `<43-char key>.json` names, at most 4 KiB, and only
content that is a pointer envelope whose ed25519 signature verifies with the key in the file name and
whose body names that key. Everything else is skipped and logged. logosctl is called without a shell.
Needs `python3-cryptography`. Keep the pointer directories root-owned and writable only by the publisher.

