#!/usr/bin/env bash
# Build the pidmon kernel for THIS machine and install it (Ubuntu/Debian).  Four steps, run one at a time
# or together:
#
#   scripts/native/install.sh config [--reconfig]   make build-native/.config from the running kernel's config
#   scripts/native/install.sh build                 compile the kernel and modules, install headers into hdr/
#   scripts/native/install.sh install               copy to /lib/modules and /boot, initramfs, GRUB   (sudo)
#   scripts/native/install.sh remove RELEASE        delete an installed kernel again                  (sudo)
#   scripts/native/install.sh all                   config, build, install
#
# Nothing here reboots, and the distro kernels are left alone, so GRUB > Advanced options still offers
# them if the new kernel does not boot.
#
# config   starts from /boot/config-$(uname -r), keeps only the modules that are loaded now (LOCALMOD=0 keeps
#          the whole distro set) and applies scripts/tools/kconf.sh: empty trusted keys, no debug info, the
#          options pidmon needs.  An existing .config is kept; --reconfig starts again.
# install  refuses when the release is the kernel running now (its modules would be replaced): build with
#          another RELSUFFIX, or set FORCE=1.  The release name is 7.3.0-rc7 + RELSUFFIX.
# remove   deletes only that release's modules and /boot files.  The running kernel needs FORCE=1.
#
#   BUILD=dir          build directory                      (default: build-native)
#   RELSUFFIX=text     release suffix, set by `config`      (default: -pidmon)
#   BASE_CONFIG=file   starting config                      (default: /boot/config-$(uname -r))
#   LOCALMOD=1|0       trim to the loaded modules           (default: 1)
#   JOBS=n             compile jobs                         (default: all CPUs)
#   DRY_RUN=1          install/remove: print the sudo commands instead of running them
#   SUDO_PW_STDIN=1    install/remove: read the sudo password from the first line of stdin
# After installing: sudo reboot, then `uname -r` should print the new release.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINUX="$ROOT/linux"; HDR="$ROOT/hdr"
BUILD="${BUILD:-$ROOT/build-native}"
KERNEL="$BUILD/arch/x86/boot/bzImage"
JOBS="${JOBS:-$(nproc)}"
RELSUFFIX="${RELSUFFIX--pidmon}"
die() { echo "error: $*" >&2; exit 1; }

PW=""
S() {   # run a command as root
  if [ "${DRY_RUN:-}" = 1 ]; then echo "+ sudo $*"
  elif [ -n "$PW" ]; then printf '%s\n' "$PW" | sudo -S -p '' "$@"
  else sudo "$@"; fi
}
sudo_init() { [ "${SUDO_PW_STDIN:-}" = 1 ] && IFS= read -r PW; S -v; }

check_tools() {
  local miss=() t p
  for t in gcc make flex bison bc cpio openssl rsync depmod update-initramfs update-grub; do
    command -v "$t" >/dev/null || miss+=("$t")
  done
  for p in build-essential libssl-dev libelf-dev libncurses-dev; do
    dpkg -s "$p" >/dev/null 2>&1 || miss+=("package:$p")
  done
  if [ ${#miss[@]} -gt 0 ]; then
    echo "missing: ${miss[*]}"
    echo "install with: sudo apt install build-essential flex bison bc cpio rsync openssl libssl-dev libelf-dev libncurses-dev kmod initramfs-tools"
    exit 1
  fi
  [ -d "$LINUX" ] || die "no kernel source at $LINUX"
}

# ---------------------------------------------------------------------------------------------- config
config() {
  check_tools
  local base="${BASE_CONFIG:-/boot/config-$(uname -r)}"
  [ "${1:-}" = --reconfig ] && rm -f "$BUILD/.config"
  mkdir -p "$BUILD"
  if [ ! -f "$BUILD/.config" ]; then
    [ -f "$base" ] || die "no starting config: $base (set BASE_CONFIG=...)"
    cp "$base" "$BUILD/.config"
    echo "starting config: $base"
    if [ "${LOCALMOD:-1}" = 1 ]; then
      lsmod > "$BUILD/lsmod.txt"
      echo "localmodconfig: keeping the $(($(wc -l < "$BUILD/lsmod.txt") - 1)) modules that are loaded now"
      # new options get their defaults; `yes` is closed early on purpose, so ignore its exit status
      (set +o pipefail; yes '' | make -s -C "$LINUX" O="$BUILD" LSMOD="$BUILD/lsmod.txt" localmodconfig >/dev/null)
    fi
  else
    echo "keeping the existing $BUILD/.config (--reconfig starts again)"
  fi
  "$ROOT/scripts/tools/kconf.sh" -s "$LINUX" -O "$BUILD" --set-str LOCALVERSION "$RELSUFFIX" --disable LOCALVERSION_AUTO
}

# ---------------------------------------------------------------------------------------------- build
build() {
  check_tools
  [ -f "$BUILD/.config" ] || die "no $BUILD/.config: run 'config' first"
  local cc=(); command -v ccache >/dev/null && cc=(CC="ccache gcc")
  make -C "$LINUX" O="$BUILD" -j"$JOBS" "${cc[@]}" bzImage modules 2>&1 | tee "$BUILD/build.log" | tail -n 5
  [ -f "$KERNEL" ] || die "build failed, see $BUILD/build.log"
  make -s -C "$LINUX" O="$BUILD" headers_install INSTALL_HDR_PATH="$HDR" >"$BUILD/headers.log" 2>&1
  echo "built: $(cat "$BUILD/include/config/kernel.release")   $KERNEL"
  echo "headers: $HDR/include"
}

# ---------------------------------------------------------------------------------------------- install
install_kernel() {
  check_tools
  [ -f "$KERNEL" ] || die "nothing built in $BUILD: run 'build' first"
  local REL STAGE="$BUILD/modroot"
  REL="$(cat "$BUILD/include/config/kernel.release")"
  if [ "$REL" = "$(uname -r)" ] && [ "${FORCE:-}" != 1 ]; then
    die "release $REL is the kernel running now. Use another RELSUFFIX with 'config', or FORCE=1."
  fi
  if command -v mokutil >/dev/null && mokutil --sb-state 2>/dev/null | grep -q 'SecureBoot enabled'; then
    echo "WARNING: Secure Boot is on; this kernel is not signed with a key the firmware trusts,"
    echo "         so it will not boot until Secure Boot is turned off or a key is enrolled."
  fi

  echo "release: $REL"; sudo_init

  echo "== modules (staged as a normal user, then copied)"
  rm -rf "$STAGE"
  make -s -C "$LINUX" O="$BUILD" modules_install INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1
  rm -f "$STAGE/lib/modules/$REL/build" "$STAGE/lib/modules/$REL/source"
  S rm -rf "/lib/modules/$REL"
  S cp -a "$STAGE/lib/modules/$REL" /lib/modules/
  S depmod "$REL"

  echo "== kernel"
  S install -m 644 "$KERNEL" "/boot/vmlinuz-$REL"
  S install -m 644 "$BUILD/System.map" "/boot/System.map-$REL"
  S install -m 644 "$BUILD/.config" "/boot/config-$REL"

  echo "== initramfs"
  S update-initramfs -c -k "$REL"

  echo "== GRUB (the highest version sorts first; check it is the one you want as default)"
  S grub-editenv /boot/grub/grubenv unset saved_entry
  S update-grub

  echo; echo "Installed $REL. Nothing was rebooted."
  echo "Reboot with 'sudo reboot' and check 'uname -r' (should be $REL)."
}

# ---------------------------------------------------------------------------------------------- remove
remove() {
  local REL="${1:-}"
  [ -n "$REL" ] || die "usage: install.sh remove RELEASE   (installed: $(ls /lib/modules | tr '\n' ' '))"
  [[ "$REL" =~ ^[0-9]+\.[0-9]+[A-Za-z0-9._+-]*$ ]] || die "'$REL' does not look like a kernel release"
  [ -e "/boot/vmlinuz-$REL" ] || [ -d "/lib/modules/$REL" ] || die "$REL is not installed"
  if [ "$REL" = "$(uname -r)" ] && [ "${FORCE:-}" != 1 ]; then
    die "$REL is the kernel running now. Reboot into another one first, or set FORCE=1."
  fi
  sudo_init
  echo "== removing $REL"
  S rm -rf "/lib/modules/$REL"
  S rm -f "/boot/vmlinuz-$REL" "/boot/System.map-$REL" "/boot/config-$REL" "/boot/initrd.img-$REL"
  S update-grub
  echo "Removed $REL."
}

case "${1:-}" in
  config)  shift; config "$@" ;;
  build)   build ;;
  install) install_kernel ;;
  remove)  shift; remove "$@" ;;
  all)     config; build; install_kernel ;;
  *)       awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; [ -z "${1:-}" ] || exit 1 ;;
esac
