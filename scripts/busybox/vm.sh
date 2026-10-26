#!/usr/bin/env bash
# Boot a kernel in QEMU with a tiny busybox userland, run one command in it, and report the result.
# This is the only script that knows about QEMU and the initramfs; tests.sh and demo.sh use it.
#
#   scripts/busybox/vm.sh --dir DIR --run 'COMMAND' [options]
#
#   --dir DIR        files for the guest: DIR/x appears as /x   (packed into the initramfs)
#   --run COMMAND    shell command the guest runs; its exit status is the result
#   --kernel FILE    kernel image to boot   (default: build-<profile>/arch/x86/boot/bzImage)
#   --check-dmesg    also fail if the guest kernel logged BUG:, WARNING:, Oops, KASAN: or a lockdep splat
#   --timeout SEC    stop the VM after SEC seconds             (default 120)
#   --mem MB         guest memory                              (default 1024)
#   --log FILE       save the console output here              (default out/vm.log)
#   --gdb            pause at start and wait for gdb on :1234
#   BUSYBOX=/path    a static busybox (default: the one on PATH)
#
# Exit status: 0 the command succeeded, 1 it failed, 2 timeout, 3 no result (panic or boot failure).
set -euo pipefail
. "$(dirname "$0")/env.sh"

DIR=""; RUN=""; CHECK=0; TIMEOUT=120; MEM=1024; LOG="$OUT/vm.log"; GDB=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) DIR="$2"; shift 2 ;;
    --run) RUN="$2"; shift 2 ;;
    --kernel) KERNEL="$2"; shift 2 ;;
    --check-dmesg) CHECK=1; shift ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --mem) MEM="$2"; shift 2 ;;
    --log) LOG="$2"; shift 2 ;;
    --gdb) GDB=1; shift ;;
    -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done
[ -d "$DIR" ] || die "--dir DIR is required"
[ -n "$RUN" ] || die "--run COMMAND is required"
[ -f "$KERNEL" ] || die "no kernel at $KERNEL: run scripts/busybox/build.sh first"
check_vm_tools

# --- 1. the guest's root file system: busybox + the files from DIR + an /init that runs the job
tmpdir; W="$TMP"
cp -a "$DIR/." "$W/"
mkdir -p "$W"/{bin,proc,sys,dev,tmp}
cp "$BB" "$W/bin/busybox"
printf '%s\n' "$RUN" > "$W/job.sh"
[ "$CHECK" = 1 ] && touch "$W/check-dmesg"
cat > "$W/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin:/sbin
/bin/busybox --install -s /bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
cd /
sh /job.sh
rc=$?
# a splat from lockdep / KASAN / atomic-sleep checks counts as failure (match the splat markers,
# not the boot banner "RCU lockdep checking is enabled")
if [ -e /check-dmesg ] && dmesg | grep -E 'BUG:|WARNING:|Oops|KASAN:|possible (circular|recursive)'; then
  echo "=== kernel complained (see above)"; rc=1
fi
echo "VM_EXIT=$rc"
reboot -f
INIT
chmod +x "$W/init"
(cd "$W" && find . | cpio -o -H newc --quiet | gzip -1) > "$W.cpio.gz"

# --- 2. boot it.  QEMU must not read the terminal: under `timeout` it would be stopped by SIGTTIN
#        and hang silently.  The guest is not interactive, so stdin is /dev/null.
accel=(-machine accel=tcg -cpu max); [ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
extra=(); [ "$GDB" = 1 ] && extra=(-s -S)
mkdir -p "$(dirname "$LOG")"
set +e
timeout "$TIMEOUT" qemu-system-x86_64 "${accel[@]}" -smp 2 -m "$MEM" \
  -kernel "$KERNEL" -initrd "$W.cpio.gz" \
  -append "console=ttyS0 panic=-1 oops=panic reboot=k loglevel=4" \
  -display none -serial stdio -monitor none -no-reboot "${extra[@]}" </dev/null | tee "$LOG"
qrc=${PIPESTATUS[0]}
set -e

# --- 3. the verdict, from the marker the guest printed
[ "$qrc" = 124 ] && { echo "RESULT: TIMEOUT (no answer in ${TIMEOUT}s)"; exit 2; }
code="$(grep -a '^VM_EXIT=' "$LOG" | tail -1 | tr -d '\r' | cut -d= -f2)"
[ -n "$code" ] || { echo "RESULT: FAIL (no result marker: panic or boot failure)"; exit 3; }
[ "$code" = 0 ] && { echo "RESULT: PASS"; exit 0; }
echo "RESULT: FAIL (the command exited with $code)"; exit 1
