#!/usr/bin/env bash
# Install the pidmon kernel built in ~/pidmon/build-ubuntu next to the distro's kernels, WITHOUT
# rebooting.
#   - modules  -> /lib/modules/<release>
#   - kernel   -> /boot/vmlinuz-<release> (+ System.map, config)
#   - initramfs-> /boot/initrd.img-<release>
#   - GRUB     -> update-grub; with GRUB_DEFAULT=0 the newest kernel (ours) is the default
# Run as a user with sudo.  With SUDO_PW_STDIN=1 the first line of stdin is the sudo password.
set -euo pipefail
B="${BUILD:-$HOME/pidmon/build-ubuntu}"; L="${LINUX:-$HOME/pidmon/linux}"; STAGE="$HOME/pidmon/modroot"
REL="$(cat "$B/include/config/kernel.release")"
PW=""; [ "${SUDO_PW_STDIN:-}" = 1 ] && IFS= read -r PW
S() { if [ -n "$PW" ]; then printf '%s\n' "$PW" | sudo -S -p '' "$@"; else sudo "$@"; fi; }
[ -f "$B/arch/x86/boot/bzImage" ] || { echo "no bzImage in $B"; exit 1; }
echo "release: $REL"; S -v

echo "== modules (staged as the normal user, then copied)"
rm -rf "$STAGE"
make -s -C "$L" O="$B" modules_install INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1
rm -f "$STAGE/lib/modules/$REL/build" "$STAGE/lib/modules/$REL/source"
S rm -rf "/lib/modules/$REL"
S cp -a "$STAGE/lib/modules/$REL" /lib/modules/
S depmod "$REL"
du -sh "/lib/modules/$REL"

echo "== kernel"
S install -m 644 "$B/arch/x86/boot/bzImage" "/boot/vmlinuz-$REL"
S install -m 644 "$B/System.map" "/boot/System.map-$REL"
S install -m 644 "$B/.config" "/boot/config-$REL"

echo "== initramfs"
S update-initramfs -c -k "$REL"

echo "== GRUB: the new kernel becomes the default (GRUB_DEFAULT=0 = newest first)"
S cp -n /etc/default/grub /etc/default/grub.pidmon.bak 2>/dev/null || true
S grub-editenv /boot/grub/grubenv unset saved_entry
S update-grub 2>&1 | tail -3
echo "== result"
grep -n "^GRUB_DEFAULT" /etc/default/grub
S awk '/^menuentry .Ubuntu. /{f=1} f&&/linux\t/{print "first entry boots: " $2; exit}' /boot/grub/grub.cfg
ls -la /boot | grep -E "$REL|initrd.img-$REL"
echo "Reboot to use it: sudo reboot"
