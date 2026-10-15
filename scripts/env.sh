# Common settings; sourced by the other scripts.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS="$ROOT"
PROFILE="${PROFILE:-tiny}"          # tiny | debug
LINUX="$ROOT/linux"                 # kernel clone (its own git repo, git-ignored here)
WORK="$ROOT/out"
BUILD="$ROOT/build-$PROFILE"
HDR="$ROOT/hdr"
ROOTFS="$ROOT/rootfs"
INITRD="$WORK/initramfs-$PROFILE.cpio.gz"
JOBS="${JOBS:-$(nproc)}"
KERNEL_REPO="${KERNEL_REPO:-https://github.com/vajradevam/linux-pidmon.git}"
KERNEL_BRANCH="${KERNEL_BRANCH:-pidmon}"
die() { echo "error: $*" >&2; exit 1; }
