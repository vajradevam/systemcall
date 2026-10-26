#!/usr/bin/env bash
# Compile the Linux kernel (with pidmon) and install its UAPI headers.  Nothing else: no VM, no tests.
#   scripts/busybox/build.sh [--reconfig]
#
#   PROFILE=tiny|debug   tiny: minimal config, fast.  debug: adds KASAN, lockdep, atomic-sleep checks.
#   JOBS=n               parallel jobs (default: all CPUs)
#   --reconfig           throw away build-<profile>/.config and generate it again
#
# Steps:  1. config   tinyconfig + kconfig/tiny.config (+ kconfig/debug.config), only if there is no .config
#         2. compile  make bzImage
#         3. headers  make headers_install into hdr/   (tests.sh and demo.sh compile against these)
# Result: build-<profile>/arch/x86/boot/bzImage   and   hdr/include/linux/pidmon.h
set -euo pipefail
. "$(dirname "$0")/env.sh"
[ "${1:-}" = -h ] || [ "${1:-}" = --help ] && { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0; }
check_build_tools
[ -d "$LINUX" ] || die "no kernel source at $LINUX"
[ "${1:-}" = --reconfig ] && rm -f "$BUILD/.config"

if [ ! -f "$BUILD/.config" ]; then
  frags=("$ROOT/kconfig/tiny.config")
  [ "$PROFILE" = debug ] && frags+=("$ROOT/kconfig/debug.config")
  mkdir -p "$BUILD"
  make -C "$LINUX" O="$BUILD" tinyconfig
  "$LINUX/scripts/kconfig/merge_config.sh" -m -O "$BUILD" "$BUILD/.config" "${frags[@]}"
  make -C "$LINUX" O="$BUILD" olddefconfig
  # Kconfig silently drops options whose dependencies are not met; say so.
  while read -r line; do
    sym="${line%%=*}"
    grep -qx "$line" "$BUILD/.config" ||
      echo "WARNING: wanted $line, got: $(grep -E "^(# )?$sym[= ]" "$BUILD/.config" || echo absent)"
  done < <(cat "${frags[@]}" | grep -E '^CONFIG_[A-Z0-9_]+=')
  echo "config written: $BUILD/.config ($PROFILE)"
else
  echo "config: using the existing $BUILD/.config (--reconfig starts again)"
fi

cc=(); command -v ccache >/dev/null && cc=(CC="ccache gcc")
make -C "$LINUX" O="$BUILD" -j"$JOBS" "${cc[@]}" bzImage 2>&1 | tee "$BUILD/build.log"
make -C "$LINUX" O="$BUILD" "${cc[@]}" headers_install INSTALL_HDR_PATH="$HDR" >"$BUILD/headers.log" 2>&1
echo "kernel:  $KERNEL"
echo "headers: $HDR/include"
