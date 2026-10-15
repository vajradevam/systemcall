#!/usr/bin/env bash
# Boot the kernel + initramfs in QEMU. Exit status 0 only if the guest printed PIDMON_EXIT=0.
#   --gdb   pause at start and listen for gdb on :1234
set -euo pipefail
. "$(dirname "$0")/env.sh"

KERNEL="$BUILD/arch/x86/boot/bzImage"
[ -f "$KERNEL" ] || die "no kernel at $KERNEL"
[ -f "$INITRD" ] || die "no initramfs at $INITRD (run 40-rootfs.sh)"

accel=(-machine accel=tcg -cpu max)
[ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
extra=(); [ "${1:-}" = --gdb ] && extra=(-s -S)
# QEMU must not read the controlling tty: under `timeout` it runs in a background process
# group and gets stopped by SIGTTIN, hanging silently. Non-interactive guest, so /dev/null.

mkdir -p "$WORK"; LOG="$WORK/run-$PROFILE.log"
set +e
timeout "${RUN_TIMEOUT:-120}" qemu-system-x86_64 "${accel[@]}" -smp 2 -m 512M \
  -kernel "$KERNEL" -initrd "$INITRD" \
  -append "console=ttyS0 panic=-1 oops=panic reboot=k loglevel=6" \
  -display none -serial stdio -monitor none -no-reboot "${extra[@]}" </dev/null | tee "$LOG"
qrc=${PIPESTATUS[0]}
set -e

[ "$qrc" = 124 ] && { echo "RESULT: TIMEOUT (hang)"; exit 2; }
if grep -q '^PIDMON_EXIT=0' "$LOG"; then echo "RESULT: PASS"; exit 0; fi
grep -q '^PIDMON_EXIT=' "$LOG" && { echo "RESULT: FAIL (tests)"; exit 1; }
echo "RESULT: FAIL (no result marker: panic or boot failure)"; exit 3
