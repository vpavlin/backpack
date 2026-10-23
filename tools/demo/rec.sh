#!/usr/bin/env bash
# rec.sh NAME SECONDS — record the emulator screen for one scene into scenes/NAME.mp4 (background-safe).
set -euo pipefail
export PATH=$HOME/Android/Sdk/platform-tools:$PATH
NAME=$1; SECS=${2:-60}; D=$(cd "$(dirname "$0")" && pwd)/scenes; mkdir -p "$D"
adb shell rm -f /sdcard/scene.mp4
adb shell screenrecord --bit-rate 6000000 --size 720x1600 --time-limit "$SECS" /sdcard/scene.mp4
adb pull -q /sdcard/scene.mp4 "$D/$NAME.mp4"
ls -la "$D/$NAME.mp4"
