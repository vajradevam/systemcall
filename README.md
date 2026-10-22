# monitor and limit your own child processes (CS-G623)

`pidmon` adds two system calls to Linux. A process can ask the kernel how much CPU, memory and
I/O one of its **descendants** (child, grandchild, ...) has used, and the kernel enforces that
descendant's resource limits when asked. Any pid outside the caller's own subtree is refused.

- Kernel base: mainline Linux 7.3.0-rc7, commit `0fcb05b72` (pinned in `BASE_COMMIT`)
- `pidmon_start` is syscall **473** and `pidmon_read` is **474** (x86-64 and i386; 472 was already taken by `fchroot`)

## How it works

```c
long pidmon_start(pid_t pid, unsigned int flags);                    /* 473 */
long pidmon_read (pid_t pid, unsigned int flags,
                  struct pidmon_usage __user *uattr, size_t usize);  /* 474 */
```

1. `pidmon_start(pid, 0)` opens a monitoring window on `pid` (`0` means the caller itself).
2. The target runs. Later, `pidmon_read(pid, flags, &usage, sizeof(usage))` returns what it used
   during the window, compares that with the target's `rlimit`s, and signals it if a limit is exceeded.

`pid` is a thread-group id in the caller's pid namespace. The target must be the caller or a
descendant, judged from the **current** parent chain at every call. If a parent dies and its child is
reparented (to init or a subreaper), the old ancestor loses access.

**Flags for `pidmon_read`:** `PIDMON_F_NO_ENFORCE` reports without signalling; `PIDMON_F_STOP` releases the
monitor after reading. `usize` lets the struct grow later, like `clone3`: at least 328 bytes, at most one page.

| Call | Errors |
|---|---|
| `pidmon_start` | `EINVAL` bad flags or pid, `ESRCH` no such process, `EPERM` not self or a descendant, `EBUSY` already monitored, `ENOSPC` 64 monitors already held, `ENOMEM`, `EINTR` |
| `pidmon_read` | `EINVAL`/`E2BIG` bad flags, pid or size, `ENOENT` no monitor for that pid, `ESRCH` target gone and nothing was kept, `EPERM` target left the caller's tree, `EFAULT` bad pointer |

### What `pidmon_read` returns

`struct pidmon_usage` (328 bytes, in `include/uapi/linux/pidmon.h`). Counters are the change since
`pidmon_start`, sizes are the value now, and peaks are measured since `pidmon_start`.

| Group | Fields |
|---|---|
| header | `size`, `state` (`EXITED`, `ALREADY_SIGNALLED`), `exceeded_soft`, `exceeded_hard`, `signal_sent` |
| time | `wall_ns` (length of the window) |
| CPU | `utime_ns`, `stime_ns`, `cpu_total_ns` (since the process began), `cpu_permille` (1000 = one CPU fully busy), `vol_ctxsw`, `invol_ctxsw` |
| page faults | `minor_faults`, `major_faults` |
| I/O | `rchar`, `wchar`, `read_bytes`, `write_bytes` |
| memory (bytes) | `rss_bytes`, `rss_anon`, `rss_file`, `rss_peak`, `vm_bytes`, `vm_peak`, `data_vm`, `stack_vm`, `locked_vm` |
| process | `nr_threads`, `nr_open_fds` |
| limits | soft and hard value of CPU, AS, RSS, DATA, STACK, MEMLOCK, NOFILE |

### Enforcement

The target's own rlimits are the limits. These seven are compared with a matching usage number
(the other rlimits have none): CPU with total CPU seconds, AS with virtual size, RSS with resident size,
DATA, STACK (main-thread stack), MEMLOCK, and NOFILE with open descriptors.

- Over a **soft** limit: `SIGXCPU` for CPU, `SIGTERM` for the rest.
- Over a **hard** limit: `SIGKILL`.
- Each severity is sent once per monitor; a later read still reports the violation but sets `ALREADY_SIGNALLED`.
- Enforcement happens when `pidmon_read` is called, not in the background.
- Signals are sent without a uid or `CAP_KILL` check: being the ancestor is the authorization.
- The kernel already enforces `RLIMIT_CPU` and `RLIMIT_AS` itself; `RLIMIT_RSS` it never enforces, so `pidmon_read` is where that one bites.

### Design notes

- **No timer.** `start` remembers the counters and `read` subtracts. Peak memory comes from the kernel's own high-water marks, reset at `start` (this also resets the target's `VmHWM` in `/proc`).
- **One monitor per target.** A second `start` on the same pid returns `EBUSY`, because resetting the peaks for one owner would corrupt another's window. An owner holds at most 64.
- **A child that already exited can still be read.** A small hook in `do_exit()` saves the final numbers of a monitored process before its memory is freed, so reading after the child was reaped still works (`EXITED` is set). If it was orphaned before it died, its numbers are withheld (`EPERM`).
- **Lifetime.** Each monitor record names its target by `struct pid`, not by task pointer, so a recycled pid number cannot be confused with it. Records hang off the owner and are freed on `PIDMON_F_STOP` or when the owner exits. Forked children do not inherit them.
- **A pid is a whole process.** CPU is summed over all its threads and memory is read once from the shared address space. A thread id that is not a process id gives `ESRCH`.

## Kernel changes

| File | Change |
|---|---|
| `kernel/pidmon.c` (new) | both syscalls, record bookkeeping, the exit hook |
| `include/uapi/linux/pidmon.h` (new) | user-visible struct, flags, limit indices |
| `include/linux/pidmon.h` (new) | the exit hook declaration |
| `kernel/exit.c` | one call to the hook in `do_exit()` |
| `include/linux/sched/signal.h` | `pidmon_recs` list head in `signal_struct` |
| `include/linux/syscalls.h`, `kernel/Makefile` | prototypes, build rule |
| `arch/x86/entry/syscalls/syscall_64.tbl`, `syscall_32.tbl`, `include/uapi/asm-generic/unistd.h`, `scripts/syscall.tbl` | syscall numbers 473 and 474 |

`./scripts/60-mkpatch.sh` writes `pidmon.patch`, which contains exactly these changes against `BASE_COMMIT`.
The test kernel config (`kconfig/tiny.config`) also turns on `TASKSTATS`, `TASK_XACCT` and
`TASK_IO_ACCOUNTING`, so the I/O fields are filled in; without them they read 0.

## Build and test

Everything is built and tested in a small QEMU virtual machine, never on the host's own kernel.
The VM boots our kernel with an initramfs that contains a **static busybox** and the test programs.
Its `/init` runs every test, checks the kernel log for `BUG:`, `WARNING:`, `Oops` and `KASAN:`
reports, and prints a verdict that the host script turns into `PASS`, `FAIL` or `TIMEOUT`.

Two kernel profiles: `tiny` (minimal config, fast) and `debug` (adds KASAN, lockdep, RCU and
atomic-sleep checks, panic on oops).

You need gcc, make, bc, flex, bison, cpio, gzip, file, qemu-system-x86_64 and a static busybox
(`./scripts/00-deps.sh` checks). KVM and ccache are optional but much faster.

```bash
./scripts/run-all.sh                    # config (first time), build, initramfs, boot, result
PROFILE=debug ./scripts/run-all.sh      # same with the debug kernel
./scripts/run-all.sh --rootfs-only      # only tests changed: skip the kernel rebuild
./scripts/50-run.sh --gdb               # boot paused, gdb on :1234
./scripts/60-mkpatch.sh                 # write pidmon.patch
```

### Tests (`tests/`)

Each test forks its own process tree, so a kill cannot hit the runner. Both profiles pass.

| Test | What it checks |
|---|---|
| `00_smoke` | the VM boots and runs a static binary |
| `10_args` | struct size, every argument error, `STOP`, oversized buffers, the 64-monitor cap |
| `20_tree` | self, child, grandchild allowed; parent, sibling, init refused; orphans refused; a subreaper keeps access |
| `30_usage` | reported CPU, faults, memory, fds and limits agree with `/proc` and `prlimit` |
| `40_window` | peaks and counters cover only the time since `start` |
| `50_enforce` | soft warning once, `NO_ENFORCE`, hard kill, NOFILE, CPU, monitoring yourself |
| `60_exit` | a zombie, a reaped child and a killed child keep their numbers; an owner exiting with monitors |
| `70_threads` | CPU summed over threads, memory counted once, a group whose main thread already exited |
| `80_ns` | pid namespaces: numbers are resolved in the caller's namespace |
| `90_stress` | threads and processes racing `start`/`read`/`STOP`/kill against exiting targets |
| `95_unpriv` | an unprivileged owner can signal a root-owned descendant that `kill(2)` would refuse |

### What ran where

| | Environment | busybox |
|---|---|---|
| All development and every test run | QEMU VM, `tiny` and `debug` kernels | yes |
| `demo/pidmon_demo.c`, first runs | the same QEMU VM (`demo/run-demo-qemu.sh`) | yes |
| `demo/pidmon_demo.c`, native run | lab machine `d153-system`, Ubuntu 24.04, booted into our kernel (built from its own distro config) | no |
| The test suite on that Ubuntu machine | not run | n/a |

So the KASAN and lockdep checks cover only the QEMU `debug` kernel. On the Ubuntu kernel we confirmed
only that the kernel log has no `BUG:`, `WARNING:` or `Oops` lines. Some tests assume the busybox VM
(for example, orphans being adopted by pid 1) and would need changes to run on a systemd machine.

## Demo

`demo/pidmon_demo.c` walks through the features and prints the results: monitoring yourself, the
ancestry rule, an orphaned grandchild, a full usage report, soft and hard limit enforcement, and
reading a child that already exited.

```bash
gcc -O1 -Wall -isystem <headers_install dir>/include -o pidmon_demo demo/pidmon_demo.c
./pidmon_demo
```

- `demo/run-demo-qemu.sh <bzImage> <headers dir>` runs it inside QEMU against any kernel build.
- `demo/install-kernel.sh` installs a kernel built in `~/pidmon/build-ubuntu` on an Ubuntu machine
  (modules, `/boot`, initramfs, `update-grub`). It needs sudo and does not reboot.
- The headers come from `make headers_install`. Without them a program can use the raw numbers 473 and 474.

## Doubts

1. Is enforcement at `pidmon_read` time enough, or must a limit be enforced in the background between calls? That would need a timer or a hook in the page-fault path.
2. Are a time-weighted average of memory and the peak CPU % in a single interval required? Both need sampling. We report totals, the window-average CPU and peak memory.
3. Is one monitor per target acceptable?

## Somethings

- When the kernel signals a process at its soft `RLIMIT_CPU`, it also raises that soft limit by one second, so by the time `pidmon_read` looks the limit may have moved. The CPU tests therefore check our verdict against the numbers we report, and use the hard limit for the deterministic case.
- Run QEMU with `</dev/null` under `timeout` (the scripts do): otherwise it stops waiting to read the terminal.
- The tiny kernel has no ACPI, so the guest ends with `reboot -f` rather than `poweroff`.
- Read next: `linux/Documentation/process/adding-syscalls.rst`, and `getrusage` in `linux/kernel/sys.c`, which this code follows.
