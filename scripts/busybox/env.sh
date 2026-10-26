# Shared settings and checks for the scripts in this directory; sourced, not run on its own.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROFILE="${PROFILE:-tiny}"                  # tiny | debug
LINUX="$ROOT/linux"                         # kernel source
OUT="$ROOT/out"                             # logs
HDR="$ROOT/hdr"                             # installed UAPI headers
BUILD="$ROOT/build-$PROFILE"                # kernel build directory
KERNEL="${KERNEL:-$BUILD/arch/x86/boot/bzImage}"
JOBS="${JOBS:-$(nproc)}"
die() { echo "error: $*" >&2; exit 1; }

# temporary files, removed when the script ends.  tmpdir sets $TMP (a subshell would hide it from the trap)
TMPS=(); trap 'rm -rf "${TMPS[@]}"' EXIT
tmpdir() { TMP="$(mktemp -d)"; TMPS+=("$TMP" "$TMP.cpio.gz"); }

need() { command -v "$1" >/dev/null || { echo "missing: $1  ($2)"; MISSING=1; }; }
check_build_tools() {
  MISSING=0
  need gcc "build-essential / base-devel"; need make make; need bc bc; need flex flex; need bison bison
  [ $MISSING = 0 ] || exit 1
}
check_vm_tools() {
  MISSING=0
  need qemu-system-x86_64 "qemu-system-x86 / qemu-full"; need cpio cpio; need gzip gzip; need file file
  BB="${BUSYBOX:-$(command -v busybox || true)}"
  if [ -z "$BB" ]; then echo "missing: busybox (pacman -S busybox | apt install busybox-static), or set BUSYBOX=/path"; MISSING=1
  elif ! file -L "$BB" | grep -q 'statically linked'; then echo "busybox at $BB is not static (apt: busybox-static)"; MISSING=1; fi
  [ $MISSING = 0 ] || exit 1
}
need_headers() { [ -f "$HDR/include/linux/pidmon.h" ] || die "no headers in $HDR: run scripts/busybox/build.sh first"; }
