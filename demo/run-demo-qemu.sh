#!/usr/bin/env bash
# Boot a kernel in QEMU with only busybox and the demo, and print the demo's output.
#   demo/run-demo-qemu.sh <bzImage> <headers_install dir containing include/>
# Works without root and without touching the host's kernel.
set -euo pipefail
KERNEL="${1:?bzImage}"; HDR="${2:?headers dir}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BB="${BUSYBOX:-$(command -v busybox)}"
file -L "$BB" | grep -q 'statically linked' || { echo "busybox at $BB is not static"; exit 1; }
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W"/{bin,proc,sys,dev,tmp}
cp "$BB" "$W/bin/busybox"
gcc -static -O1 -Wall -pthread -isystem "$HDR/include" -o "$W/bin/pidmon_demo" "$HERE/pidmon_demo.c"
cat > "$W/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin
/bin/busybox --install -s /bin
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
/bin/pidmon_demo
echo "DEMO_EXIT=$?"
reboot -f
INIT
chmod +x "$W/init"
(cd "$W" && find . | cpio -o -H newc 2>/dev/null | gzip) > "$W.cpio.gz"
accel=(-machine accel=tcg -cpu max); [ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
timeout "${RUN_TIMEOUT:-120}" qemu-system-x86_64 "${accel[@]}" -smp 2 -m 1G -kernel "$KERNEL" -initrd "$W.cpio.gz" \
  -append "console=ttyS0 panic=-1 reboot=k quiet loglevel=3" -display none -serial stdio -monitor none -no-reboot </dev/null
rm -f "$W.cpio.gz"
