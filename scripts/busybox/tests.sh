#!/usr/bin/env bash
# Compile the test programs and run them in the VM, as root.
#   scripts/busybox/tests.sh [NAME ...]      NAME limits it to tests whose file name contains NAME
#                                            e.g.  tests.sh 52_ enforce
# Fails too if the guest kernel logs BUG:, WARNING:, Oops, KASAN: or a lockdep splat.
# Needs build.sh first.    PROFILE=tiny|debug  picks the kernel build (default tiny).
# Result: PASS / FAIL.  The console log is out/tests-<profile>.log.
set -euo pipefail
. "$(dirname "$0")/env.sh"
[ "${1:-}" = -h ] || [ "${1:-}" = --help ] && { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0; }
need_headers

srcs=()
for f in "$ROOT"/tests/*.c; do
  if [ $# -eq 0 ]; then srcs+=("$f"); continue; fi
  for n in "$@"; do [[ "$(basename "$f")" == *"$n"* ]] && { srcs+=("$f"); break; }; done
done
[ ${#srcs[@]} -gt 0 ] || die "no test matches: $*"

tmpdir; S="$TMP"; mkdir -p "$S/tests"
# -isystem: our freshly installed headers must win over /usr/include/linux
for f in "${srcs[@]}"; do
  gcc -static -pthread -O1 -g -Wall -Wextra -isystem "$HDR/include" -o "$S/tests/$(basename "${f%.c}")" "$f"
done
echo "compiled ${#srcs[@]} test(s)"
cp "$ROOT/tests/run.sh" "$S/run-tests"
"$(dirname "$0")/vm.sh" --dir "$S" --run '/bin/busybox sh /run-tests /tests' \
     --check-dmesg --log "$OUT/tests-$PROFILE.log"
