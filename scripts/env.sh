# Common settings; sourced by the other scripts.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS="$ROOT"
PROFILE="${PROFILE:-tiny}"          # tiny | debug
LINUX="$ROOT/linux"                 # kernel source, a plain folder in this repo
WORK="$ROOT/out"
BUILD="$ROOT/build-$PROFILE"
HDR="$ROOT/hdr"
ROOTFS="$ROOT/rootfs"
INITRD="$WORK/initramfs-$PROFILE.cpio.gz"
JOBS="${JOBS:-$(nproc)}"
die() { echo "error: $*" >&2; exit 1; }
