#!/usr/bin/env bash
# Build the initramfs: static busybox + static test binaries from tests/*.c.
set -euo pipefail
. "$(dirname "$0")/env.sh"

BB="${BUSYBOX:-$(command -v busybox || true)}"
[ -n "$BB" ] || die "busybox not found (sudo pacman -S busybox)"
file -L "$BB" | grep -q 'statically linked' || die "$BB is not static"
[ -d "$HDR/include" ] || die "run 30-build.sh first (headers_install)"

rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{bin,sbin,proc,sys,dev,tmp,tests} "$WORK"
cp "$BB" "$ROOTFS/bin/busybox"

for src in "$HARNESS"/tests/*.c; do
  [ -e "$src" ] || continue
  # -isystem: our freshly installed UAPI headers must win over the host's /usr/include/linux
  gcc -static -pthread -O1 -g -Wall -Wextra -isystem "$HDR/include" -o "$ROOTFS/tests/$(basename "${src%.c}")" "$src"
done

cat > "$ROOTFS/init" <<'EOF'
#!/bin/busybox sh
export PATH=/bin:/sbin
/bin/busybox --install -s /bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
fail=0
for t in /tests/*; do
  echo "=== $t"
  "$t" || { echo "=== $t FAILED (rc=$?)"; fail=1; }
done
# any splat from lockdep / KASAN / atomic-sleep checks counts as failure (match splat markers, not
# the boot banner "RCU lockdep checking is enabled")
if dmesg | grep -E 'BUG:|WARNING:|Oops|KASAN:|possible (circular|recursive)'; then
  echo "=== kernel complained (see above)"; fail=1
fi
echo "PIDMON_EXIT=$fail"
reboot -f
EOF
chmod +x "$ROOTFS/init"

(cd "$ROOTFS" && find . | cpio -o -H newc --quiet | gzip -9) > "$INITRD"
echo "initramfs: $INITRD ($(du -h "$INITRD" | cut -f1))"
