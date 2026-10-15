#!/usr/bin/env bash
# Produce pidmon.patch: our kernel changes vs. the pinned mainline base (for handing in).
set -euo pipefail
. "$(dirname "$0")/env.sh"
BASE="$(cat "$ROOT/BASE_COMMIT")"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
git -C "$T" init -q
git -C "$T" fetch -q --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$BASE"
git -C "$T" checkout -q FETCH_HEAD
rsync -a --delete --exclude .git "$LINUX/" "$T/"
git -C "$T" add -A
git -C "$T" diff --cached --binary > "$ROOT/pidmon.patch"
git -C "$T" diff --cached --stat | tail -15
