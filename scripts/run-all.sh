#!/usr/bin/env bash
# Full loop: fetch (if needed) -> config (if needed) -> build -> rootfs -> run.
#   PROFILE=tiny|debug  ./scripts/run-all.sh [--rootfs-only]   (rootfs-only: skip kernel rebuild)
set -euo pipefail
d="$(dirname "$0")"; . "$d/env.sh"
"$d/00-deps.sh"; "$d/10-fetch.sh"
[ -f "$BUILD/.config" ] || "$d/20-config.sh"
[ "${1:-}" = --rootfs-only ] || "$d/30-build.sh"
"$d/40-rootfs.sh"
"$d/50-run.sh"
