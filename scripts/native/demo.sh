#!/usr/bin/env bash
# Compile the demo (or another program that uses the calls) and run it on THIS machine, which must be
# running a pidmon kernel.
#   scripts/native/demo.sh [FILE.c]      default demo/pidmon_demo.c; docs/examples/guard.c is the small example
# Compiles against hdr/ (made by `native/install.sh build` or `busybox.sh build`); if hdr/ is missing it
# is made from the kernel source in linux/.  Scenario 7 of the demo wants `mlock` of 4 MB to be allowed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HDR="$ROOT/hdr"
die() { echo "error: $*" >&2; exit 1; }
TMPS=(); trap 'rm -rf "${TMPS[@]}"' EXIT
command -v gcc >/dev/null || die "gcc is missing"
SRC="${1:-$ROOT/demo/pidmon_demo.c}"
[ -f "$SRC" ] || die "no such file: $SRC"

if [ ! -f "$HDR/include/linux/pidmon.h" ]; then
  echo "making headers in $HDR"
  B="$(mktemp -d)"; TMPS+=("$B")
  make -s -C "$ROOT/linux" O="$B" headers_install INSTALL_HDR_PATH="$HDR" >/dev/null
fi

S="$(mktemp -d)"; TMPS+=("$S")
gcc -pthread -O1 -Wall -isystem "$HDR/include" -o "$S/prog" "$SRC"
echo "compiled $SRC; running on $(uname -r)"
"$S/prog"
