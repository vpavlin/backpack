#!/usr/bin/env bash
# Restart backpack-hub when the announcer is stuck: no Store check for 30 min, or nothing confirmed in
# Store for 2.5 h (healthy = every note re-sent and re-confirmed hourly). Log in watchdog.log.
F=/home/vpavlin/.backpack/announcer.json; now=$(date +%s)
systemctl --user is-active -q backpack-hub || exit 0          # stopped on purpose, or systemd restarting it
up=$(( now - $(date -d "$(systemctl --user show backpack-hub -p ActiveEnterTimestamp --value)" +%s) ))
[ "$up" -lt 1800 ] && exit 0                                   # give a fresh start half an hour
why=""
if [ ! -f "$F" ]; then why="no announcer.json"
else
  read c l < <(python3 -I -c "import json; d=json.load(open('$F')); print(d.get('checkedAt',0), d.get('lastConfirmedAt',0))")
  [ $(( now - c )) -gt 1800 ] && why="no Store check for $(( (now - c) / 60 )) min"
  [ -z "$why" ] && [ $(( now - l )) -gt 9000 ] && why="nothing confirmed in Store for $(( (now - l) / 60 )) min"
fi
[ -z "$why" ] && exit 0
echo "$(date -u +%FT%TZ) $why -> restarting backpack-hub" >> /home/vpavlin/backpack-hub/watchdog.log
systemctl --user restart backpack-hub
