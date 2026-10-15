#!/usr/bin/env bash
# Generate $BUILD/.config = tinyconfig + kconfig/tiny.config (+ debug.config for PROFILE=debug).
set -euo pipefail
. "$(dirname "$0")/env.sh"
[ -d "$LINUX" ] || die "run 10-fetch.sh first"

frags=("$HARNESS/kconfig/tiny.config")
[ "$PROFILE" = debug ] && frags+=("$HARNESS/kconfig/debug.config")

mkdir -p "$BUILD"
make -C "$LINUX" O="$BUILD" tinyconfig
"$LINUX/scripts/kconfig/merge_config.sh" -m -O "$BUILD" "$BUILD/.config" "${frags[@]}"
make -C "$LINUX" O="$BUILD" olddefconfig

# Kconfig silently drops options whose dependencies are unmet; report them.
missing=0
while read -r line; do
  sym="${line%%=*}"
  grep -qx "$line" "$BUILD/.config" || { echo "WARNING: wanted $line, got: $(grep -E "^(# )?$sym[= ]" "$BUILD/.config" || echo absent)"; missing=1; }
done < <(cat "${frags[@]}" | grep -E '^CONFIG_[A-Z0-9_]+=')
[ $missing = 0 ] && echo "config OK ($PROFILE)"
exit 0
