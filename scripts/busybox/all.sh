#!/usr/bin/env bash
# Build the kernel, then run the tests, then the demo, in the VM.  It stops at the first step that fails.
#   scripts/busybox/all.sh          PROFILE=tiny|debug  picks the kernel build (default tiny)
set -euo pipefail
d="$(dirname "$0")"
"$d/build.sh"
"$d/tests.sh"
"$d/demo.sh"
