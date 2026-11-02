#!/usr/bin/env bash
# logosctl against the backpack-hub session (the headless Backpack announcer)
set -a; . /home/vpavlin/backpack-hub/env; set +a
cd /home/vpavlin/port-0.3/logosctl && exec ./logosctl "$@"
