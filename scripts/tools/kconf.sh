#!/usr/bin/env bash
# Change kernel config options in a build directory's .config, then let kconfig sort out the rest.
# It only needs a kernel source tree (for scripts/config) and a .config, so it works on any machine.
#
#   kconf.sh [-s SRC] [-O BUILD] [--no-defaults] [ACTION OPTION [VALUE]]...
#
#   -s SRC     kernel source tree             (default: ../linux next to this script)
#   -O BUILD   build dir holding .config      (default: SRC, i.e. an in-tree build)
#
# Extra actions are applied after the built-in list below, in the order given:
#   --enable OPT         --disable OPT        --module OPT
#   --set-str OPT TEXT   --set-val OPT NUMBER
# OPT is written without the CONFIG_ prefix.
#
# Examples:
#   kconf.sh -s ~/linux -O ~/linux/build                     # just the built-in fixes
#   kconf.sh -O build --enable TASK_XACCT --set-str LOCALVERSION -test
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../linux"; BUILD=""; DEFAULTS=1; EXTRA=()

while [ $# -gt 0 ]; do
  case "$1" in
    -s) SRC="$2"; shift 2 ;;
    -O) BUILD="$2"; shift 2 ;;
    --no-defaults) DEFAULTS=0; shift ;;
    --enable|--disable|--module) EXTRA+=("$1" "$2"); shift 2 ;;
    --set-str|--set-val) EXTRA+=("$1" "$2" "$3"); shift 3 ;;
    -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (try --help)" >&2; exit 1 ;;
  esac
done

SRC="$(cd "$SRC" && pwd)"
BUILD="$(cd "${BUILD:-$SRC}" && pwd)"
CFG="$BUILD/.config"
[ -x "$SRC/scripts/config" ] || { echo "no $SRC/scripts/config: is $SRC a kernel tree?" >&2; exit 1; }
[ -f "$CFG" ] || { echo "no $CFG: copy a starting config there first (for example /boot/config-\$(uname -r))" >&2; exit 1; }

c() { "$SRC/scripts/config" --file "$CFG" "$@"; }

if [ "$DEFAULTS" = 1 ]; then
  # --- Ubuntu/Debian configs point these at certificate files that are not in a plain source tree,
  #     and the build stops with "No rule to make target debian/canonical-certs.pem".
  c --set-str SYSTEM_TRUSTED_KEYS ""
  c --set-str SYSTEM_REVOCATION_KEYS ""

  # --- Debug info makes the build several times bigger and slower, and BTF needs pahole.
  # Since DEBUG_INFO became a choice, "--disable DEBUG_INFO" alone is undone by olddefconfig on Ubuntu
  # (DWARF5 is selected there).  Turn the DWARF choices off and pick the "none" one.
  c --disable DEBUG_INFO_DWARF5
  c --disable DEBUG_INFO_DWARF4
  c --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
  c --enable DEBUG_INFO_NONE
  c --disable DEBUG_INFO              # also removes DEBUG_INFO_DWARF*
  c --disable DEBUG_INFO_BTF          # also removes the pahole dependency
  c --disable GDB_SCRIPTS

  # --- Needed by pidmon: without them the I/O fields of the report read 0.
  c --enable TASKSTATS
  c --enable TASK_XACCT
  c --enable TASK_IO_ACCOUNTING
fi

# user actions, three words for the --set-* forms and two for the others
i=0
while [ $i -lt ${#EXTRA[@]} ]; do
  act="${EXTRA[$i]}"; opt="${EXTRA[$((i+1))]}"
  case "$act" in
    --set-str|--set-val) c "$act" "$opt" "${EXTRA[$((i+2))]}"; i=$((i+3)) ;;
    *)                   c "$act" "$opt"; i=$((i+2)) ;;
  esac
done

# add the options these changes switch on or off, with their defaults
make -s -C "$SRC" O="$BUILD" olddefconfig

echo "kconf: $CFG updated"
for o in SYSTEM_TRUSTED_KEYS SYSTEM_REVOCATION_KEYS DEBUG_INFO DEBUG_INFO_BTF GDB_SCRIPTS TASK_XACCT TASK_IO_ACCOUNTING; do
  grep -E "^(# )?CONFIG_$o[= ]" "$CFG" || echo "CONFIG_$o: absent"
done
