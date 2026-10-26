# monitor and limit your own child processes (CS-G623)

`pidmon` adds two system calls to Linux. A process opens a **window** on one of its descendants
(child, grandchild, ...) and hands the kernel the resource limits for that window. While the window
is open the kernel watches the descendant and warns or kills it if it goes over a limit. The second
call reads how much CPU, memory and I/O the descendant used. Any pid outside the caller's own
subtree is refused.

Full usage reference: [docs/pidmon.md](docs/pidmon.md).

- Kernel base: mainline Linux 7.3.0-rc7, commit `0fcb05b72` (pinned in `BASE_COMMIT`)
- `pidmon_start` is syscall **473** and `pidmon_read` is **474** (x86-64 and i386; 472 was already taken by `fchroot`)

## The two calls

```c
long pidmon_start(pid_t pid, unsigned int flags,
                  const struct pidmon_limits __user *limits, size_t size);   /* 473 */
long pidmon_read (pid_t pid, unsigned int flags,
                  struct pidmon_usage __user *usage, size_t size);           /* 474 */
```

`pid` is a thread-group id in the caller's pid namespace; `0` means the caller itself. The target must
be the caller or a descendant, judged from the **current** parent chain at every call. If a parent dies
and its child is reparented (to init or a subreaper), the old ancestor loses access.

| | `pidmon_start` | `pidmon_read` |
|---|---|---|
| Job | open the window, remember the counters, arm the limits | report the window; **never signals** |
| Flags | `PIDMON_F_NO_ENFORCE`: judge and report, never signal | `PIDMON_F_STOP`: close the window after reading |
| Struct | `struct pidmon_limits`, 120 bytes, or `NULL` with size 0 | `struct pidmon_usage`, at least 328 bytes, at most one page |

`size` lets both structs grow later, like `clone3`: a longer struct is accepted if the extra tail is zero
(`E2BIG` otherwise), and a longer buffer on `read` gets a zeroed tail.

| Call | Errors |
|---|---|
| `pidmon_start` | `EINVAL` bad flags, pid, size or limits (unknown limit bit, soft above hard, size without a struct), `E2BIG` size too big or unknown non-zero tail, `EFAULT` bad pointer, `ESRCH` no such process, or one that has finished exiting (a zombie), `EPERM` not self or a descendant, `EBUSY` already monitored, `ENOSPC` 64 monitors already held, `ENOMEM`, `EINTR` |
| `pidmon_read` | `EINVAL`/`E2BIG` bad flags, pid or size, `ENOENT` no monitor for that pid, `ESRCH` target gone and nothing was kept, `EPERM` target left the caller's tree, `EFAULT` bad pointer |

## Limits

`struct pidmon_limits` carries a `set` bitmask and a soft/hard pair for each of seven limits. Each is
compared with a usage number in the unit of the rlimit of the same name:

| Index | Limit | Compared with |
|---|---|---|
| `PIDMON_LIM_CPU` | `RLIMIT_CPU` | total CPU seconds since the process began |
| `PIDMON_LIM_AS` | `RLIMIT_AS` | virtual size |
| `PIDMON_LIM_RSS` | `RLIMIT_RSS` | resident size |
| `PIDMON_LIM_DATA` | `RLIMIT_DATA` | private writable mappings |
| `PIDMON_LIM_STACK` | `RLIMIT_STACK` | main-thread stack mapping |
| `PIDMON_LIM_MEMLOCK` | `RLIMIT_MEMLOCK` | `mlock`ed memory |
| `PIDMON_LIM_NOFILE` | `RLIMIT_NOFILE` | open file descriptors |

- Bit *n* of `set` means `lim[n]` is given. Any limit not given is the target's own `RLIMIT_*` of that
  name, read afresh at each check. With no struct at all, every limit comes from the target's rlimits.
- `~0` means unlimited. The soft value must not exceed the hard value.
- The limits belong to the window. The target's real rlimits are never changed, so no privilege is
  needed to give a limit above the target's own hard limit.

## Enforcement

Enforcement runs for as long as the window is open: from `pidmon_start` until the target exits or
`pidmon_read` with `PIDMON_F_STOP`. Nobody has to call `pidmon_read`.

- Over a **soft** limit: one warning, `SIGXCPU` for CPU and `SIGTERM` for the rest.
- Over a **hard** limit: `SIGKILL`. A target that was warned and later crosses its hard limit is still killed.
- Each severity is sent once per window.
- A target that is already over a limit when the window opens is acted on at the first check.
- A window cannot be opened on a process that has already finished exiting, even if it is still an unreaped zombie (`ESRCH`); the window that was open when it exited stays readable.
- A window started with `PIDMON_F_NO_ENFORCE` records the breaches but sends nothing.
- Signals are sent without a uid or `CAP_KILL` check: being the ancestor is the authorization.
- The check runs about every 10 ms (`PIDMON_POLL_MS`), so a breach is acted on within roughly that time.
- Enforcement stops for a target that left the owner's tree, and for all of an owner's windows when the owner exits.
- The kernel itself enforces `RLIMIT_CPU` and `RLIMIT_AS` on a target that has them set; `RLIMIT_RSS` it never enforces.

## What `pidmon_read` returns

`struct pidmon_usage` (328 bytes, in `include/uapi/linux/pidmon.h`). Counters are the change since
`pidmon_start`, sizes are the value now, and peaks are measured since `pidmon_start`.

| Group | Fields |
|---|---|
| header | `size`, `state` (`EXITED`), `exceeded_soft`, `exceeded_hard`, `signal_sent` |
| time | `wall_ns` (length of the window) |
| CPU | `utime_ns`, `stime_ns`, `cpu_total_ns` (since the process began), `cpu_permille` (1000 = one CPU fully busy), `vol_ctxsw`, `invol_ctxsw` |
| page faults | `minor_faults`, `major_faults` |
| I/O | `rchar`, `wchar`, `read_bytes`, `write_bytes` |
| memory (bytes) | `rss_bytes`, `rss_anon`, `rss_file`, `rss_peak`, `vm_bytes`, `vm_peak`, `data_vm`, `stack_vm`, `locked_vm` |
| process | `nr_threads`, `nr_open_fds` |
| limits | soft and hard value of each limit in force, in `PIDMON_LIM_*` order |

`exceeded_soft` and `exceeded_hard` are bitmasks of `PIDMON_LIM_*`, and `signal_sent` is the strongest
signal sent. All three cover the **whole window**: a limit that was breached stays reported after the
target has dropped back under it. `state` has `PIDMON_ST_EXITED` once the target is gone and the numbers are final.

## Design notes

- **Counters, gauges, peaks.** `start` remembers the counters and `read` subtracts. Gauges are read when asked.
  Peak memory comes from the kernel's own high-water marks, reset at `start` (this also resets the target's `VmHWM` in `/proc`).
- **A sampler, only while needed.** A delayed work item judges every live window each 10 ms. It is queued
  by `pidmon_start` and requeues itself only while some window is live, so an idle system pays nothing.
- **One monitor per target.** A second `start` on the same pid returns `EBUSY`, because resetting the peaks
  for one owner would corrupt another's window. An owner holds at most 64.
- **A child that already exited can still be read.** A small hook in `do_exit()` saves the final numbers
  and the window's verdict before the memory is freed, so reading after the child was reaped still works.
  If it was orphaned before it died, its numbers are withheld (`EPERM`).
- **Lifetime.** Each record names its target by `struct pid`, not by task pointer, so a recycled pid number
  cannot be confused with it. Records hang off the owner and are freed on `PIDMON_F_STOP` or when the owner
  exits. Forked and exec'd children do not inherit them; threads of the owner share them.
- **A pid is a whole process.** CPU is summed over all its threads and memory is read once from the shared
  address space. A thread id that is not a process id gives `ESRCH`.

## Kernel changes

| File | Change |
|---|---|
| `kernel/pidmon.c` (new) | both syscalls, record bookkeeping, the sampler, the exit hook |
| `include/uapi/linux/pidmon.h` (new) | user-visible structs, flags, limit indices |
| `include/linux/pidmon.h` (new) | the exit hook declaration |
| `kernel/exit.c` | one call to the hook in `do_exit()` |
| `include/linux/sched/signal.h` | `pidmon_recs` list head in `signal_struct` |
| `include/linux/syscalls.h`, `kernel/Makefile` | prototypes, build rule |
| `arch/x86/entry/syscalls/syscall_64.tbl`, `syscall_32.tbl`, `include/uapi/asm-generic/unistd.h`, `scripts/syscall.tbl` | syscall numbers 473 and 474 |

`./scripts/tools/make-patch.sh` writes `pidmon.patch`, which contains exactly these changes against `BASE_COMMIT`.
The test kernel config (`kconfig/tiny.config`) also turns on `TASKSTATS`, `TASK_XACCT` and
`TASK_IO_ACCOUNTING`, so the I/O fields are filled in; without them they read 0.

## Build and test

The scripts are grouped by where they run. Each one prints its options with `--help`.

| Script | Job |
|---|---|
| `scripts/busybox/build.sh` | compile the kernel and install its headers into `hdr/` |
| `scripts/busybox/tests.sh` | compile the tests and run them in the QEMU + busybox VM |
| `scripts/busybox/demo.sh` | compile the demo and run it in the VM |
| `scripts/busybox/all.sh` | build, tests, demo, one after the other |
| `scripts/busybox/vm.sh` | the VM itself: busybox initramfs, QEMU, result. Used by `tests.sh` and `demo.sh` |
| `scripts/busybox/env.sh` | settings shared by the others (sourced, not run) |
| `scripts/native/install.sh` | build and install the kernel on this Ubuntu machine: `config`, `build`, `install`, `remove`, `all` |
| `scripts/native/tests.sh` | run the tests on this machine, which must be running a pidmon kernel |
| `scripts/native/demo.sh` | run the demo on this machine, which must be running a pidmon kernel |
| `scripts/tools/kconf.sh` | change options in any kernel `.config` (used by `native/install.sh`, usable anywhere) |
| `scripts/tools/make-patch.sh` | write `pidmon.patch`, the kernel changes against `BASE_COMMIT` |

```bash
# In the VM (nothing touches this machine's kernel)
./scripts/busybox/build.sh             # config (first time), compile, headers into hdr/
./scripts/busybox/tests.sh             # all tests in the VM
./scripts/busybox/tests.sh 52_ enforce # only tests whose name contains 52_ or enforce
./scripts/busybox/demo.sh              # the demo in the VM
./scripts/busybox/demo.sh docs/examples/guard.c    # any other program that uses the calls
./scripts/busybox/all.sh               # build, tests, demo
PROFILE=debug ./scripts/busybox/all.sh # the debug kernel: KASAN, lockdep, atomic-sleep checks

# On a real Ubuntu machine
./scripts/native/install.sh all        # config, build, install; then reboot
./scripts/native/tests.sh              # after rebooting into the new kernel
./scripts/native/demo.sh
```

`vm.sh` packs a static busybox and the compiled programs into an initramfs, boots the kernel with it
in QEMU, and runs one command. For the tests it also checks the kernel log for `BUG:`, `WARNING:`,
`Oops` and `KASAN:` reports. The console log is saved in `out/`.

Two kernel profiles: `tiny` (minimal config, fast) and `debug` (adds KASAN, lockdep, RCU and
atomic-sleep checks, panic on oops).

For the VM you need gcc, make, bc, flex, bison, cpio, gzip, file, qemu-system-x86_64 and a static busybox.
KVM and ccache are optional but much faster.

### Installing the kernel on a real Ubuntu machine

`native/install.sh` starts from the running kernel's config, keeps the modules that are loaded now, and applies
`tools/kconf.sh`, which sets what Ubuntu's config needs (empty `SYSTEM_TRUSTED_KEYS` and
`SYSTEM_REVOCATION_KEYS`, no debug info). It installs next to the distro kernels (modules, `/boot`,
initramfs, GRUB), never reboots, and `remove RELEASE` deletes a kernel again. `kconf.sh` can also apply any
other change: `scripts/tools/kconf.sh -s linux -O build --enable TASK_XACCT --set-str LOCALVERSION -test`.

### Tests (`tests/`)

Each test forks its own process tree, so a kill cannot hit the runner. All 26 pass on both profiles.

| Test | What it checks |
|---|---|
| `00_smoke` | the VM boots and runs a static binary |
| `10_args` | struct sizes, every argument error for both calls and the limits struct, `STOP`, oversized buffers, the 64-monitor cap |
| `15_unlimited` | unlimited limits read as `~0` and never trip; given limits are reported exactly |
| `20_tree` | self, child, grandchild allowed; parent, sibling, init refused; orphans refused; a subreaper keeps access |
| `30_usage` | reported CPU, faults, memory, fds and limits agree with `/proc` and `prlimit` |
| `35_memory` | `mlock`, private mappings and freeing show in the gauges; peaks stay |
| `40_window` | peaks and counters cover only the time since `start` |
| `45_io` | `rchar` and `wchar` cover the window only |
| `50_enforce` | warning once and kill with no read, `NO_ENFORCE`, given limits vs the target's own, a breach kept after recovery, nothing outside the window, NOFILE, CPU, monitoring yourself |
| `52_boundary` | a limit is breached only above it: exactly at the limit is not over |
| `53_equal_limits` | soft equal to hard kills at once and reports both severities |
| `54_live_rlimit` | an unset limit is the target's own rlimit, re-read as it changes |
| `55_escalation` | warn then kill, several limits at once, hard beats soft |
| `56_ignored_warning` | a handled warning is sent once; the target survives and signal_sent says so |
| `58_sampler` | breach latency, independent windows with different limits, no enforcement after the owner exits or for an orphan |
| `60_exit` | a zombie, a reaped child and a killed child keep their numbers; an owner exiting with monitors |
| `62_new_window_clean` | a new window starts with empty exceeded masks and signal_sent |
| `65_scope` | threads share records; fork, exec and descendants do not |
| `68_restart` | re-arming after `STOP`, a zombie keeps its numbers and refuses a new window, no slot is lost, slot reuse at the cap |
| `70_threads` | CPU summed over threads, memory counted once, a group whose main thread already exited |
| `72_ctxsw` | voluntary context switches are counted in the window |
| `75_owner_exit` | an exiting owner frees all its records, repeated against the cap |
| `80_ns` | pid namespaces: numbers are resolved in the caller's namespace |
| `85_rate` | `cpu_permille` for idle, one and two busy threads; `wall_ns` against the clock |
| `90_stress` | threads and processes racing `start`/`read`/`STOP`/kill against exiting targets and the sampler |
| `95_unpriv` | an unprivileged owner can signal a root-owned descendant that `kill(2)` would refuse, and set any limit |

### Where it has been run

The current design (limits at `start`, enforcement in the window) has been built and tested in the
QEMU VM as root, on both the `tiny` and `debug` kernels, and the demo has been run there. The KASAN and
lockdep checks cover only the QEMU `debug` kernel.

It has also been run natively on an Arch Linux machine (systemd, 8 CPUs, kernel built on the machine
and booted from a unified kernel image). All 26 test programs pass as root, and the demo runs to the
end both as root and as an unprivileged user. The three tests that count minor page faults (`30_usage`,
`40_window` and `60_exit`) fail when transparent hugepages are set to `always`; with `madvise` or
`never` they pass. It has not been run on Ubuntu.

## Demo

`demo/pidmon_demo.c` is a guided tour of the whole interface in 15 scenarios, each printing what it
asks and how the kernel answers:

| # | Scenario |
|---|---|
| 1 | monitoring yourself |
| 2 | who may be monitored: child and grandchild yes; parent, init, nonexistent no; an orphan loses access, unless you are a subreaper |
| 3 | a full report: every field of `struct pidmon_usage`, with CPU burnt before the window left out of the counters |
| 4 | memory: gauges follow `mmap`, `mlock` and `munmap`; peaks stay |
| 5 | limits: the target's own by default, or the ones given in the struct, with the target's real rlimits untouched |
| 6 | enforcement with no read: one warning at the soft limit, a kill at the hard limit |
| 7 | all seven limits, one child each, with the signal each gets and how fast |
| 8 | report-only windows (`PIDMON_F_NO_ENFORCE`) |
| 9 | a breach is remembered after the target recovers |
| 10 | enforcement exists only inside the window |
| 11 | a child that exited, even as an unreaped zombie, keeps its numbers; no new window on it |
| 12 | threads: CPU is summed, a thread id is not a valid target |
| 13 | monitors belong to their owner; a forked child starts with none |
| 14 | the cap of 64 monitors |
| 15 | every error the two calls can return |

```bash
./scripts/busybox/demo.sh          # in the VM
./scripts/native/demo.sh           # on a machine that runs a pidmon kernel
```

- The demo is compiled against the headers in `hdr/` (from `busybox/build.sh` or `native/install.sh build`). Without them a program can
  use the raw numbers 473 and 474.
- Scenario 7 needs a system that allows `mlock` of 4 MB; if not, that row says the action was refused.

## Notes

- When the kernel signals a process at its own soft `RLIMIT_CPU`, it also raises that soft limit by one second.
  Limits given to `pidmon_start` are not touched by that, so the CPU tests give their limits in the struct.
- The kernel's peak-memory marks come from approximate per-CPU counters and can be about 100 KB behind a gauge reading.
- `RLIMIT_NOFILE` cannot be set to unlimited (it is capped by `fs.nr_open`), so the unlimited tests leave it out.
- Run QEMU with `</dev/null` under `timeout` (the scripts do): otherwise it stops waiting to read the terminal.
- The tiny kernel has no ACPI, so the guest ends with `reboot -f` rather than `poweroff`.
- Read next: `linux/Documentation/process/adding-syscalls.rst`, and `getrusage` in `linux/kernel/sys.c`, which this code follows.
