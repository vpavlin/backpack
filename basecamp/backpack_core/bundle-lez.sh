#!/usr/bin/env bash
# bundle-lez.sh <in.lgx> <out.lgx> <variant>=<libbackpack_access_ffi.so>[:sha256] ...
# Adds the LEZ membership library (backpack-access ffi, portable build) next to backpack_core_plugin.so in
# each named variant, as libbackpack_access_ffi.so. The core dlopens it from there (bp_lez.hpp), so a
# package without it still loads; only repos that use LEZ need it.
# Portable builds and their recipe: vpavlin/backpack-access ffi/PORTABLE-BUILD.md (no RUNPATH, glibc 2.28).
#   e.g. bundle-lez.sh result-portable/*.lgx out.lgx linux-amd64=ffi-amd64.so:057b9d… linux-arm64=ffi-arm64.so:62ed57…
set -euo pipefail
LGX=${LGX:-lgx}
in=$1 out=$2; shift 2
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
cp "$in" "$t/pkg.lgx"; chmod u+w "$t/pkg.lgx"
tar xzf "$t/pkg.lgx" -C "$t" variants
for spec in "$@"; do
  v=${spec%%=*} rest=${spec#*=}; lib=${rest%%:*}; want=""; [[ $rest == *:* ]] && want=${rest#*:}
  [ -d "$t/variants/$v" ] || { echo "no variant $v in $in" >&2; exit 1; }
  if [ -n "$want" ]; then got=$(sha256sum "$lib" | cut -d' ' -f1); [ "$got" = "$want" ] || { echo "$lib: sha256 $got, expected $want" >&2; exit 1; }; fi
  if readelf -d "$lib" | grep -q -E 'RUNPATH|RPATH|libpcsclite'; then echo "$lib: not a portable build (RUNPATH or pcsclite)" >&2; exit 1; fi
  cp "$lib" "$t/variants/$v/libbackpack_access_ffi.so"
  "$LGX" add "$t/pkg.lgx" --variant "$v" --files "$t/variants/$v" --main backpack_core_plugin.so -y >/dev/null
  echo "$v: + libbackpack_access_ffi.so ($(sha256sum "$lib" | cut -c1-12))"
done
"$LGX" verify "$t/pkg.lgx" >/dev/null
cp "$t/pkg.lgx" "$out"
