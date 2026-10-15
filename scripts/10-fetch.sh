#!/usr/bin/env bash
# Clone the kernel (our fork, branch pidmon) into linux/ if it is not there yet.
set -euo pipefail
. "$(dirname "$0")/env.sh"
if [ ! -d "$LINUX/.git" ]; then
  git clone --depth 1 -b "$KERNEL_BRANCH" "$KERNEL_REPO" "$LINUX"
fi
echo "kernel: $(git -C "$LINUX" log -1 --format='%h %s')  (base $(cat "$ROOT/BASE_COMMIT" | cut -c1-9))"
