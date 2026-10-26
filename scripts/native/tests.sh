#!/usr/bin/env bash
# Compile the test programs and run them on THIS machine, which must be running a pidmon kernel.
#   scripts/native/tests.sh [NAME ...]       NAME limits it to tests whose file name contains NAME
# Run it as root for the full set (95_unpriv drops to an ordinary user by itself).
# Compiles against hdr/ (made by `native/install.sh build` or `busybox.sh build`); if hdr/ is missing it
# is made from the kernel source in linux/.  Three tests count minor page faults and fail when transparent
# hugepages are `always`:  echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
# Result: a line per test, a summary, and exit status 0 only if all passed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HDR="$ROOT/hdr"
die() { echo "error: $*" >&2; exit 1; }
TMPS=(); trap 'rm -rf "${TMPS[@]}"' EXIT
command -v gcc >/dev/null || die "gcc is missing"

if [ ! -f "$HDR/include/linux/pidmon.h" ]; then
  echo "making headers in $HDR"
  B="$(mktemp -d)"; TMPS+=("$B")
  make -s -C "$ROOT/linux" O="$B" headers_install INSTALL_HDR_PATH="$HDR" >/dev/null
fi

srcs=()
for f in "$ROOT"/tests/*.c; do
  if [ $# -eq 0 ]; then srcs+=("$f"); continue; fi
  for n in "$@"; do [[ "$(basename "$f")" == *"$n"* ]] && { srcs+=("$f"); break; }; done
done
[ ${#srcs[@]} -gt 0 ] || die "no test matches: $*"

S="$(mktemp -d)"; TMPS+=("$S")
mkdir -p "$S/tests"
for f in "${srcs[@]}"; do   # -isystem: our headers must win over /usr/include/linux
  gcc -pthread -O1 -g -Wall -Wextra -isystem "$HDR/include" -o "$S/tests/$(basename "${f%.c}")" "$f"
done
echo "compiled ${#srcs[@]} test(s); running on $(uname -r)"
"$ROOT/tests/run.sh" "$S/tests"
