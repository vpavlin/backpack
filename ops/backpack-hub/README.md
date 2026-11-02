# backpack-hub: backpack_core headless under logosctl

The same `backpack_core` Basecamp runs, without a GUI, in its own logosctl session. With
`BACKPACK_DIR` pointing at the repos folder (the one the `backpack` CLI and Basecamp publish into), it
announces every repo's update note over Logos Delivery **until Store holds it**:

- a changed `latest.json` (published by anyone: the CLI, Basecamp, CI) is sent within a minute;
- each sent note is looked up in Store; until it is there it is sent again (90 s, 3, 5, 10, then every
  15 min), and the reason is kept (`<repos>/announcer.json`, and the repo page in Basecamp);
- a confirmed note is sent again every hour (`BACKPACK_RESEND_SEC`), so it never ages out of Store.

Robustness:
- `backpack-hub.service` runs the daemon in the foreground with `Restart=always`: a crash is back in ~15 s.
- `backpack-hub-watchdog.timer` (every 10 min) restarts it when the announcer is stuck: nothing
  confirmed in Store for 2.5 h (a hung Delivery node keeps the process alive; with every note
  confirmed the core is quiet until the hourly re-send, so "no check for a while" is normal).

Set up (paths are the crib's; adjust):

```sh
H=/home/vpavlin/backpack-hub; mkdir -p $H/home $H/cfg; cp env ctl.sh load.sh watchdog.sh $H/
$H/ctl.sh daemon start --detach
for p in delivery_module loam_core ble_mesh; do $H/ctl.sh install logos-$p-module.lgx -y; done
$H/ctl.sh install logos-backpack_core-module.lgx -y; $H/ctl.sh daemon stop
cp backpack-hub.service backpack-hub-watchdog.* ~/.config/systemd/user/
systemctl --user daemon-reload; systemctl --user enable --now backpack-hub backpack-hub-watchdog.timer
```

Keep delivery_module at a version proven here before a hub uses it (0.3.0 as of 2026-11-02; 0.3.2
crashed the VPS hub's logosctl every few minutes).
