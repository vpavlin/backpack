#!/usr/bin/env bash
# Rebuild the Android arm64 Logos Storage libraries that the Scala app ships:
#   arm64-v8a/libstorage.so            the Nim node (logos-storage-nim, fetch-only build)
#   arm64-v8a/liblogos_storage_jni.so  the JNI glue (android/jni/logos_storage_ffi.c)
#   arm64-v8a/libc++_shared.so         from the NDK
# and record where they came from in PROVENANCE. The .so files are gitignored; this script and
# PROVENANCE are the record. Run it from anywhere; it only writes into this directory and $SRC.
#
# Environment (all optional):
#   SRC              logos-storage-nim checkout (default ~/.cache/scala-libstorage/logos-storage-nim)
#   REPO_URL         where to clone it from (default: the fork holding the build task)
#   REF              commit to build (default below; must contain the libStorageAndroid task)
#   ANDROID_NDK_HOME NDK root (default ~/Android/Sdk/ndk/27.1.12297006)
#   JOBS             parallel jobs for `make update` (default 2; the full build peaks ~2 GB RAM)
#
# Build cost: ~5 min and ~2 GB RAM on an i5/8 GB box. Don't run it next to a gradle release build.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO_URL=${REPO_URL:-https://github.com/vpavlin/nim-codex.git}
REF=${REF:-6af20491addbf8edaec68a1d51db5c6abe94c0c9}   # android-fetch-client: libStorageAndroid task
SRC=${SRC:-$HOME/.cache/scala-libstorage/logos-storage-nim}
NDK=${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.1.12297006}
JOBS=${JOBS:-2}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
OUT=$HERE/arm64-v8a

[ -x "$TC/aarch64-linux-android30-clang" ] || { echo "NDK not found at $NDK (set ANDROID_NDK_HOME)" >&2; exit 1; }

# 1. Source at the pinned commit.
if [ ! -d "$SRC/.git" ]; then
  git clone "$REPO_URL" "$SRC"
fi
if ! git -C "$SRC" cat-file -e "$REF^{commit}" 2>/dev/null; then
  git -C "$SRC" fetch --quiet --no-recurse-submodules "$REPO_URL" "$REF" ||
    git -C "$SRC" fetch --quiet --no-recurse-submodules --all
fi
if [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no --ignore-submodules=all)" ]; then
  echo "$SRC has local changes outside submodules; refusing to build from a dirty tree" >&2; exit 1
fi
git -C "$SRC" checkout --quiet --detach "$REF"
git -C "$SRC" submodule update --init --recursive

# 2. Patches to vendored dependencies: patches/<submodule>/*.patch, applied inside vendor/<submodule>.
#    Idempotent: an already-applied patch is skipped; one that applies neither way stops the build.
PATCHES=()
for dir in "$HERE"/patches/*/; do
  [ -d "$dir" ] || continue
  sub=$(basename "$dir")
  for p in "$dir"*.patch; do
    [ -f "$p" ] || continue
    if git -C "$SRC/vendor/$sub" apply --check "$p" 2>/dev/null; then
      git -C "$SRC/vendor/$sub" apply "$p"; echo "applied  $sub/$(basename "$p")"
    elif git -C "$SRC/vendor/$sub" apply --reverse --check "$p" 2>/dev/null; then
      echo "present  $sub/$(basename "$p")"
    else
      echo "patch does not apply: $sub/$(basename "$p")" >&2; exit 1
    fi
    PATCHES+=("$sub/$(basename "$p") $(sha256sum "$p" | cut -c1-16)")
  done
done

# 3. Toolchain bootstrap (pinned Nim) on a fresh checkout, then the Android build task.
[ -x "$SRC/vendor/nimbus-build-system/vendor/Nim/bin/nim" ] || make -C "$SRC" -j"$JOBS" update
( cd "$SRC" && ANDROID_NDK_HOME="$NDK" \
    NDK_CLANG="$TC/aarch64-linux-android30-clang" NDK_CLANGXX="$TC/aarch64-linux-android30-clang++" \
    ./env.sh nim libStorageAndroid build.nims )

# 4. Install: strip the node, copy the NDK C++ runtime, relink the JNI glue against the new node.
mkdir -p "$OUT"
"$TC/llvm-strip" --strip-unneeded -o "$OUT/libstorage.so" "$SRC/build/android/arm64-v8a/libstorage.so"
cp "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/"
"$TC/aarch64-linux-android30-clang" -shared -fPIC -O2 -I"$HERE/android/jni" \
  "$HERE/android/jni/logos_storage_ffi.c" -L"$OUT" -lstorage -llog -o "$OUT/liblogos_storage_jni.so"

# 5. Provenance (committed; the .so files are not).
{
  echo "# Written by build-libstorage.sh — do not edit by hand."
  echo "source      $REPO_URL @ $(git -C "$SRC" rev-parse HEAD)"
  echo "nim-libp2p  $(git -C "$SRC/vendor/nim-libp2p" rev-parse HEAD) (before patches)"
  for p in "${PATCHES[@]:-}"; do [ -n "$p" ] && echo "patch       $p"; done
  echo "ndk         $(basename "$NDK")"
  echo "built       $(date -u +%Y-%m-%dT%H:%MZ)"
  for f in libstorage.so liblogos_storage_jni.so libc++_shared.so; do
    echo "sha256      $(sha256sum "$OUT/$f" | cut -c1-16)  $f"
  done
} > "$HERE/PROVENANCE"
cat "$HERE/PROVENANCE"
