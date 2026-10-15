#!/usr/bin/env bash
# Check host tools. Prints what is missing and exits non-zero if anything is.
set -uo pipefail
. "$(dirname "$0")/env.sh"
miss=0
need() { command -v "$1" >/dev/null || { echo "missing: $1  ($2)"; miss=1; }; }
need gcc "build-essential / base-devel"; need make make; need bc bc; need flex flex; need bison bison
need cpio cpio; need gzip gzip; need file file; need qemu-system-x86_64 "qemu-system-x86 / qemu-full"
need pahole "dwarves (only for BTF; harmless)"; command -v ccache >/dev/null || echo "note: ccache not found (optional, speeds rebuilds)"
BB="${BUSYBOX:-$(command -v busybox || true)}"
if [ -z "$BB" ]; then echo "missing: busybox (pacman -S busybox | apt install busybox-static); or set BUSYBOX=/path"; miss=1
elif ! file -L "$BB" | grep -q 'statically linked'; then echo "busybox at $BB is not static (apt: busybox-static)"; miss=1; fi
[ -w /dev/kvm ] || echo "note: /dev/kvm not writable; QEMU will use slow TCG emulation"
:
[ $miss = 0 ] && echo "deps OK"
exit $miss
