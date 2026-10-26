#!/bin/sh
# Run every program in a directory, one after the other; print one line per test and a summary.
# Exit status is 1 if any test failed.  Used inside the VM and, with --native, on the host.
#   tests/run.sh DIR
dir="${1:?usage: run.sh DIR}"
n=0; failed=""
for t in "$dir"/*; do
  [ -f "$t" ] && [ -x "$t" ] || continue
  n=$((n + 1)); name="$(basename "$t")"
  echo "=== $name"
  "$t"; rc=$?
  [ $rc = 0 ] || { echo "=== $name FAILED (rc=$rc)"; failed="$failed $name"; }
done
if [ -n "$failed" ]; then echo "=== $n tests, failed:$failed"; exit 1; fi
echo "=== $n tests, all passed"
