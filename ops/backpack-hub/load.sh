#!/usr/bin/env bash
# ExecStartPost: wait for the daemon, then load backpack_core (its dependencies load with it)
for i in $(seq 1 60); do /home/vpavlin/backpack-hub/ctl.sh status >/dev/null 2>&1 && break; sleep 2; done
/home/vpavlin/backpack-hub/ctl.sh module load backpack_core
