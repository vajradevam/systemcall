#!/usr/bin/env bash
# Compile the demo (or another program that uses the calls) and run it in the VM.
#   scripts/busybox/demo.sh [FILE.c]      default demo/pidmon_demo.c; docs/examples/guard.c is the small example
# Needs build.sh first.    PROFILE=tiny|debug  picks the kernel build (default tiny).
# The console log is out/demo-<profile>.log.
set -euo pipefail
. "$(dirname "$0")/env.sh"
[ "${1:-}" = -h ] || [ "${1:-}" = --help ] && { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0; }
need_headers

src="${1:-$ROOT/demo/pidmon_demo.c}"
[ -f "$src" ] || die "no such file: $src"
name="$(basename "${src%.c}")"
tmpdir; S="$TMP"; mkdir -p "$S/bin"
gcc -static -pthread -O1 -Wall -isystem "$HDR/include" -o "$S/bin/$name" "$src"
echo "compiled $src"
"$(dirname "$0")/vm.sh" --dir "$S" --run "/bin/$name" --log "$OUT/demo-$PROFILE.log"
