#!/usr/bin/env bash
# Rebuild liblogos_storage_jni.so from android/jni/logos_storage_ffi.c (Backpack's copy: carries the
# timeout use-after-free fix — a timed-out call marks its Resp abandoned and the late callback frees
# it). libstorage.so comes from logos-storage-nim PR #1540 — PROVENANCE has the recipe (ADR 0005).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NDK=${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.1.12297006}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -f "$HERE/arm64-v8a/libstorage.so" ] || { echo "build libstorage.so first (PROVENANCE)" >&2; exit 1; }
"$TC/aarch64-linux-android30-clang" -shared -fPIC -O2 -Wall -I"$HERE/android/jni" "$HERE/android/jni/logos_storage_ffi.c" \
  -L"$HERE/arm64-v8a" -lstorage -llog -o "$HERE/arm64-v8a/liblogos_storage_jni.so"
sha256sum "$HERE"/arm64-v8a/*.so
