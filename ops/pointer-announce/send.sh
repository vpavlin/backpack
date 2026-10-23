#!/usr/bin/env bash
# Announce Backpack repo pointers over Logos Delivery (backpack ADR 0007).
# The pointer files are the signed envelopes the CLI rsyncs here; this sends each one, verbatim, to
# /backpack/1/<repo key>/json through the scala hub's delivery node (logos.test, cluster 2), where
# phones (Loam shared node) receive it live and fleet Store keeps it for catch-up.
# Usage: send.sh [KEY…]   (no args: every pointer file, which the daily timer does to keep Store warm)
set -u
CTL=/root/scala-hub2/ctl.sh
DIRS="/srv/data/public/backpack /srv/data/unlisted/backpack"
send() {
  local f=$1 key; key=$(basename "$f" .json)
  out=$("$CTL" call delivery_module send "str:/backpack/1/$key/json" "str:$(cat "$f")" 2>&1)
  echo "$(date -Is) $key $out"
}
if [ $# -gt 0 ]; then
  for k in "$@"; do for d in $DIRS; do [ -f "$d/$k.json" ] && send "$d/$k.json"; done; done
else
  for d in $DIRS; do for f in "$d"/*.json; do [ -f "$f" ] && send "$f"; done; done
fi
exit 0
