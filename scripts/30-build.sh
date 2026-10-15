#!/usr/bin/env bash
# Build bzImage for $PROFILE and install the UAPI headers the tests compile against.
set -euo pipefail
. "$(dirname "$0")/env.sh"
[ -f "$BUILD/.config" ] || die "run 20-config.sh first"

CC_ARG=(); command -v ccache >/dev/null && CC_ARG=(CC="ccache gcc")
make -C "$LINUX" O="$BUILD" -j"$JOBS" "${CC_ARG[@]}" bzImage 2>&1 | tee "$BUILD/build.log"
make -C "$LINUX" O="$BUILD" "${CC_ARG[@]}" headers_install INSTALL_HDR_PATH="$HDR" >"$BUILD/headers.log" 2>&1
echo "built: $BUILD/arch/x86/boot/bzImage"
