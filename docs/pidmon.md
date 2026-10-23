# pidmon: using the system calls

`pidmon` is a pair of Linux system calls that let a process **watch its own descendants and hold them
to resource limits**. A process opens a *window* on one of its children (or grandchildren, ...), tells
the kernel which limits apply, and from then on the kernel warns or kills the descendant if it goes
over. A second call reads how much CPU, memory and I/O the descendant used.

This document is the reference for programs that call them. For how they are built into the kernel
see the [README](../README.md); for a guided run see `demo/pidmon_demo.c`.

Contents: [Synopsis](#synopsis) · [Concepts](#concepts) · [pidmon_start](#pidmon_start) ·
[Limits](#limits) · [Enforcement](#enforcement) · [pidmon_read](#pidmon_read) ·
[The usage report](#the-usage-report) · [Life of a window](#life-of-a-window) ·
[Who may be monitored](#who-may-be-monitored) · [Errors](#errors) · [Recipes](#recipes) ·
[Pitfalls](#pitfalls) · [Compatibility](#compatibility-and-growing-the-structs) · [Complete example](#complete-example)

## Synopsis

```c
#include <linux/pidmon.h>          /* from the kernel's headers_install */
#include <sys/syscall.h>
#include <unistd.h>

long pidmon_start(pid_t pid, unsigned int flags,
                  const struct pidmon_limits *limits, size_t size);     /* 473 */
long pidmon_read (pid_t pid, unsigned int flags,
                  struct pidmon_usage *usage, size_t size);             /* 474 */
```

There is no libc wrapper. Call them through `syscall(2)`:

```c
#ifndef __NR_pidmon_start
#define __NR_pidmon_start 473
#endif
#ifndef __NR_pidmon_read
#define __NR_pidmon_read 474
#endif

syscall(__NR_pidmon_start, pid, 0, NULL, 0);
syscall(__NR_pidmon_read,  pid, 0, &usage, sizeof(usage));
```

Both return `0` on success. On failure `syscall()` returns `-1` and sets `errno`
(see [Errors](#errors)). The numbers are the same on x86-64 and i386.

The caller needs no privilege and no capability.

## Concepts

**Target.** The process you monitor. `pid` names a whole process (a thread-group id), as seen in your
pid namespace. A thread id that is not also a process id is refused. `pid == 0` means yourself.

**Owner.** The process that called `pidmon_start`. Every monitor belongs to one owner (the whole
process, all its threads together).

**Window.** The interval from `pidmon_start` until it is closed. The window has a fixed set of limits,
a start time, and a snapshot of the target's counters taken at the start. The window is also the unit
of enforcement and of reporting.

**Record (monitor).** The kernel object that holds a window. One live record per target, at most 64
per owner. It survives the target's death so the final numbers can be read, and is freed when you read
it with `PIDMON_F_STOP` or when the owner exits.

**Counters, gauges, peaks.**

| Kind | Meaning | Examples |
|---|---|---|
| counter | change since `pidmon_start` | `utime_ns`, `minor_faults`, `rchar`, context switches |
| gauge | value at the moment of the read | `rss_bytes`, `vm_bytes`, `nr_open_fds`, `nr_threads` |
| peak | highest value since `pidmon_start` | `rss_peak`, `vm_peak` |

## pidmon_start

```c
long pidmon_start(pid_t pid, unsigned int flags,
                  const struct pidmon_limits *limits, size_t size);
```

Opens a window on `pid`, takes the baseline, arms the limits, and starts enforcing.

| Argument | Meaning |
|---|---|
| `pid` | target process id, or `0` for yourself. Must not be negative |
| `flags` | `0`, or `PIDMON_F_NO_ENFORCE` (judge and report, never signal) |
| `limits` | the limits to apply, or `NULL` to use the target's own rlimits for all seven |
| `size` | `sizeof(*limits)`, or `0` when `limits` is `NULL` |

What `pidmon_start` does to the target: it resets the target's peak memory marks (this also resets
`VmHWM` in `/proc/<pid>/status`), and nothing else. It never changes the target's rlimits.

`pidmon_start(pid, 0, NULL, 0)` is the shortest valid call: watch with the target's own limits and
enforce them.

A start on a process that has already finished exiting fails with `ESRCH`, even if it is still an
unreaped zombie, because there is nothing left to watch.

### struct pidmon_limits

```c
struct pidmon_rlim {
	__aligned_u64 soft;       /* warn above this */
	__aligned_u64 hard;       /* kill above this */
};

struct pidmon_limits {
	__u32 set;                /* bit n set: lim[n] is given */
	__u32 __pad;              /* ignored; keep it 0 */
	struct pidmon_rlim lim[PIDMON_NLIM];     /* 7 entries, 120 bytes in all */
};
```

Zero the whole struct first (`struct pidmon_limits lim = {};`), set the bits for the limits you want,
and fill in those entries. Entries whose bit is not set are ignored.

## Limits

Seven limits, each the target's `RLIMIT_*` of the same name and compared in the same unit:

| Index | Constant | Mirrors | Compared with | Unit |
|---|---|---|---|---|
| 0 | `PIDMON_LIM_CPU` | `RLIMIT_CPU` | total CPU time of the process since it began (all threads) | seconds |
| 1 | `PIDMON_LIM_AS` | `RLIMIT_AS` | virtual size | bytes |
| 2 | `PIDMON_LIM_RSS` | `RLIMIT_RSS` | resident size (anon + file + shared) | bytes |
| 3 | `PIDMON_LIM_DATA` | `RLIMIT_DATA` | private writable mappings | bytes |
| 4 | `PIDMON_LIM_STACK` | `RLIMIT_STACK` | main-thread stack mapping | bytes |
| 5 | `PIDMON_LIM_MEMLOCK` | `RLIMIT_MEMLOCK` | `mlock`ed memory | bytes |
| 6 | `PIDMON_LIM_NOFILE` | `RLIMIT_NOFILE` | open file descriptors | count |

Rules:

- **Given or inherited.** A limit whose bit is set in `set` comes from your struct. Every other limit
  is the target's own `RLIMIT_*`, read again at every check. So if the target lowers its own
  `RLIMIT_RSS` mid-window, the new value applies.
- **Unlimited.** `~0ULL` (that is, `RLIM_INFINITY` as 64 bits) means no limit. A target whose limits
  are all unlimited is never judged over, however much it uses.
- **Order.** `soft <= hard`, otherwise `EINVAL`. `soft == hard` is allowed; crossing it kills at once,
  with no warning step.
- **Boundary.** CPU is breached at *or above* the limit (like the kernel's own `RLIMIT_CPU`). All
  others are breached only *above* the limit: sitting exactly at the limit is not over.
- **The target's rlimits are never changed.** The values belong to the window only. Because of this,
  you can give a limit above the target's own hard limit and no privilege is needed.
- **Unknown bits.** A bit above bit 6 in `set` is `EINVAL`.
- **Reported back.** `pidmon_read` returns the limits in force, in the same `pidmon_rlim` layout, so
  you can see exactly which values were judged.

## Enforcement

Enforcement runs for as long as the window is open, whether or not anyone calls `pidmon_read`. The
kernel checks every live window about every 10 ms, so a breach is acted on within roughly that time.

| Situation | Action |
|---|---|
| a limit is above its **soft** value | one warning: `SIGXCPU` for CPU, `SIGTERM` for the other six |
| a limit is above its **hard** value | `SIGKILL` |
| warned earlier, now above a hard value | `SIGKILL` still comes; a warning does not use up the kill |
| several limits breached at once | the strongest signal wins; hard beats soft |

Details:

- **Once per severity.** Each of the warning and the kill is sent at most once per window. A target
  that handles or ignores the warning is not warned again and is not escalated on its own.
- **Already over at the start.** A target that is over a limit when the window opens is acted on at
  the first check, about 10 ms later.
- **Report only.** With `PIDMON_F_NO_ENFORCE` nothing is ever sent; breaches are still recorded and
  reported.
- **`pidmon_read` never signals.** It only reports.
- **No permission check on the signal.** Being the target's ancestor is the authorization. The owner may
  signal a descendant that `kill(2)` would refuse (for example one with a different uid).
- **Enforcement stops** when: the target exits; the window is closed with `PIDMON_F_STOP`; the target
  leaves the owner's tree (it was orphaned and reparented away); or the owner exits.
- **It is a monitor, not a hard cap.** Between two checks a target can briefly exceed a limit. A fast
  allocator can reach well past the limit in 10 ms. If you need a cap that can never be passed, set the
  real rlimit on the child before it runs (`setrlimit`) as well.
- **The kernel's own enforcement is separate.** `RLIMIT_CPU` and `RLIMIT_AS` are enforced by the kernel
  itself on a target that has them set; `RLIMIT_RSS` never is. pidmon adds the missing ones and a
  uniform soft/hard behavior.

## pidmon_read

```c
long pidmon_read(pid_t pid, unsigned int flags,
                 struct pidmon_usage *usage, size_t size);
```

Reports the window on `pid`. It does not change the window, signal anything, or reset any counter, so
you may call it as often as you like. Each read subtracts the same baseline, so counters grow over the
life of the window.

| Argument | Meaning |
|---|---|
| `pid` | the same pid you passed to `pidmon_start` (`0` for yourself) |
| `flags` | `0`, or `PIDMON_F_STOP`: after a successful read, close the window and free the record |
| `usage` | buffer for the report |
| `size` | `sizeof(*usage)`; at least 328, at most one page (4096) |

`size` must be at least `PIDMON_USAGE_SIZE_VER0` (328). A larger buffer is accepted and the part the
kernel does not fill is zeroed. On return `usage->size` holds the number of bytes the kernel filled in.
If the call fails (including `EFAULT`), nothing is released: `PIDMON_F_STOP` only takes effect on
success.

A read works on a target that has exited and even on one that was already reaped: the window keeps its
final numbers until you release it.

## The usage report

`struct pidmon_usage` is 328 bytes. The `__aligned_u64` fields are identical on 32- and 64-bit.

| Group | Field | Meaning |
|---|---|---|
| header | `size` | bytes the kernel filled in |
| | `state` | `PIDMON_ST_EXITED` (bit 0) once the target is gone; the numbers are then final |
| | `exceeded_soft` | bitmask of limits that went above their soft value at any time in the window |
| | `exceeded_hard` | same for hard values |
| | `signal_sent` | strongest signal pidmon sent in the window (`SIGXCPU`, `SIGTERM`, `SIGKILL`) or `0` |
| time | `wall_ns` | length of the window so far (frozen at exit) |
| CPU | `utime_ns`, `stime_ns` | user and system CPU in the window, all threads |
| | `cpu_total_ns` | user + system CPU since the process began (what `PIDMON_LIM_CPU` compares) |
| | `cpu_permille` | CPU over wall time in the window; 1000 = one CPU fully busy, 2000 = two |
| | `vol_ctxsw`, `invol_ctxsw` | voluntary and involuntary context switches in the window |
| faults | `minor_faults`, `major_faults` | in the window |
| I/O | `rchar`, `wchar` | bytes handed to read- and write-type syscalls |
| | `read_bytes`, `write_bytes` | bytes fetched from and sent to storage |
| memory (bytes) | `rss_bytes` | resident now = `rss_anon` + `rss_file` + shared memory |
| | `rss_anon`, `rss_file` | the parts of it |
| | `rss_peak` | highest resident size since the start |
| | `vm_bytes`, `vm_peak` | virtual size now, and its peak since the start |
| | `data_vm`, `stack_vm`, `locked_vm` | private writable, main-thread stack, `mlock`ed |
| process | `nr_threads`, `nr_open_fds` | now |
| limits | `lim[7]` | soft/hard of each limit in force, indexed by `PIDMON_LIM_*` |

Points worth knowing:

- **Masks are sticky.** `exceeded_soft`, `exceeded_hard` and `signal_sent` cover the whole window. A
  limit that was breached stays reported after the target dropped back under it, and after it exited.
  They are decided by what was seen at the 10 ms checks and at your reads.
- **Bit n of a mask is limit n.** Test with `mask & (1U << PIDMON_LIM_RSS)`.
- **I/O fields.** `rchar`/`wchar` need the kernel's task I/O accounting and `read_bytes`/`write_bytes`
  need block-layer accounting; they read zero when the kernel was built without it. `read_bytes` is
  zero for data served from the page cache.
- **Peaks are approximate by a small margin.** They come from the kernel's own high-water marks, which
  are batched per CPU, so `rss_peak` can sit slightly (about a megabyte) below a value you sampled.
- **Window vs process lifetime.** All counters are for the window except `cpu_total_ns`, which is for the
  whole life of the process. CPU spent before `pidmon_start` is in `cpu_total_ns` but not in `utime_ns`
  or `stime_ns`.
- **Threads.** CPU is the sum over all threads; memory is read once from the shared address space.
  Stack growth of non-main threads is counted in `data_vm`, not `stack_vm`.
- **Frozen at exit.** After the target exits, every number stops changing, including `wall_ns`.

## Life of a window

```
 pidmon_start ──► open ──────────────────────────────┐
                   │  enforcement every ~10 ms        │
                   │  pidmon_read: report only        │
                   ▼                                  │
        target exits  (state = EXITED, numbers frozen,│ pidmon_read(PIDMON_F_STOP)
        enforcement stops, record kept for reading)   │
                   │                                  │
                   └──────────────────────────────────┴──► record freed
                       (also freed when the owner exits)
```

- **Re-arming.** After `PIDMON_F_STOP` you may call `pidmon_start` on the same target again. This opens
  a fresh window: new baseline, new peaks, empty breach masks, empty `signal_sent`, and a new set of
  limits.
- **A second start on a live record** is `EBUSY`. There is one live record per target, because
  resetting the peak marks for one owner would corrupt another's window. Stop the old one first.
- **Records hold a slot** until stopped, even after the target dies. Release finished windows with
  `PIDMON_F_STOP` or you will reach the cap of 64 (`ENOSPC`).
- **A zombie** keeps the window that was open when it died. You can read it, but you cannot open a new
  window on it (`ESRCH`).
- **Owner exit.** When the owner (the whole process) exits, every record it holds is freed and
  enforcement for those windows ends. Nothing is left behind in the kernel.
- **Inheritance.** A forked child or an exec'd program starts with no records. Threads of the owner
  share the owner's records: a window started in one thread can be read or stopped from another.
- **Pid reuse is safe.** A record names its target by its kernel pid object, not by number, so a
  recycled pid number is never mistaken for the old target.

## Who may be monitored

The target must be the caller or a descendant of the caller (child, grandchild, ...), judged from the
**current** parent chain at *every* call, not just at the start.

| Target | `pidmon_start` | Why |
|---|---|---|
| yourself (`pid` 0 or your own pid) | allowed | |
| your child, grandchild, ... | allowed | |
| your parent, a sibling, `init`, an unrelated process | `EPERM` | not in your subtree |
| a pid that does not exist | `ESRCH` | |
| a thread id that is not a process id | `ESRCH` | a pid is a whole process |

Reparenting matters. If a descendant's parent dies, the descendant is adopted by `init` (or by the
nearest *subreaper*) and is no longer in the old ancestor's subtree:

- the old ancestor can no longer start, read or stop that window on a live orphan (`EPERM`), and the
  kernel stops enforcing on it;
- once the orphan dies, one read frees the record (and returns `EPERM`);
- a process that marks itself a subreaper with `prctl(PR_SET_CHILD_SUBREAPER, 1)` keeps the access:
  orphans in its subtree are adopted by it and remain monitored.

Unprivileged use works. The authority comes from ancestry alone, not from uid or capabilities: an
unprivileged owner can monitor and signal a descendant that `kill(2)` would refuse to touch (for
example one that became root through a setuid binary). Pid namespaces are respected: `pid` is
resolved in the caller's namespace, and a descendant in a child namespace is reached by the number it
has in yours.

Monitoring yourself is legal, and yourself can be killed by your own limits. For example, a start on
`pid 0` with a tiny `AS` soft limit and no `NO_ENFORCE` will terminate the caller. Use
`PIDMON_F_NO_ENFORCE` when you only want to measure yourself.

## Errors

### pidmon_start

| errno | Cause |
|---|---|
| `EINVAL` | unknown flag (or a read flag), negative `pid`, a struct with `size` smaller than 120 (including 0), a non-zero `size` with a `NULL` struct, unknown bit in `set`, soft above hard |
| `E2BIG` | `size` larger than a page, or larger than the kernel's struct with a non-zero byte in the extra part |
| `EFAULT` | `limits` is not a readable address |
| `ESRCH` | no such process, or it has already finished exiting (a zombie) |
| `EPERM` | the target is not yourself or one of your descendants |
| `EBUSY` | the target already has a live window (stop it first) |
| `ENOSPC` | you already hold 64 records |
| `ENOMEM` | out of memory |
| `EINTR` | interrupted while waiting (retry) |

### pidmon_read

| errno | Cause |
|---|---|
| `EINVAL` | unknown flag (or a start flag), negative `pid`, `size` smaller than 328 |
| `E2BIG` | `size` larger than one page |
| `EFAULT` | `usage` is not a writable address; the window is left open |
| `ENOENT` | you hold no window for that pid (never started, already stopped, or the pid is not the one you used) |
| `ESRCH` | the target is gone and no final numbers were kept |
| `EPERM` | the target has left your tree (was reparented away), or it was orphaned before it died |

On `EPERM` for an orphan that has already died, the record is freed by that read.

## Recipes

### Warn a task early and kill it if it keeps growing

```c
struct pidmon_limits lim = {};

lim.set = 1U << PIDMON_LIM_RSS;
lim.lim[PIDMON_LIM_RSS].soft = 256UL << 20;       /* SIGTERM above 256 MB */
lim.lim[PIDMON_LIM_RSS].hard = 512UL << 20;       /* SIGKILL above 512 MB */
if (syscall(__NR_pidmon_start, child, 0, &lim, sizeof(lim)))
	perror("pidmon_start");
```

In the child, install a `SIGTERM` handler that frees caches; the kill only comes if that is not enough.

### CPU budget with the usual signal

```c
lim.set = 1U << PIDMON_LIM_CPU;
lim.lim[PIDMON_LIM_CPU].soft = 10;                /* SIGXCPU after 10 s of CPU */
lim.lim[PIDMON_LIM_CPU].hard = 15;                /* SIGKILL after 15 s */
```

The time is CPU seconds of the whole process since it began, not since `pidmon_start`.

### Measure only: no signals

```c
syscall(__NR_pidmon_start, child, PIDMON_F_NO_ENFORCE, NULL, 0);
/* ... later ... */
syscall(__NR_pidmon_read, child, PIDMON_F_STOP, &u, sizeof(u));
printf("peak rss %llu, cpu %llu ns\n", u.rss_peak, u.utime_ns + u.stime_ns);
```

### Use the target's own rlimits

```c
setrlimit(RLIMIT_NOFILE, &(struct rlimit){ 200, 400 });   /* in the child, before exec */
/* in the parent */
syscall(__NR_pidmon_start, child, 0, NULL, 0);            /* enforces 200 / 400 on its fds */
```

Note that for `NOFILE`, `AS` and `CPU` the kernel enforces the same rlimit as well.

### Know afterwards what happened

```c
struct pidmon_usage u;

waitpid(child, &status, 0);
syscall(__NR_pidmon_read, child, PIDMON_F_STOP, &u, sizeof(u));
if (u.exceeded_hard & (1U << PIDMON_LIM_RSS))
	puts("killed for memory");
else if (u.signal_sent == SIGTERM)
	puts("warned, finished anyway");
```

### Poll a long-running job

```c
for (;;) {
	if (syscall(__NR_pidmon_read, child, 0, &u, sizeof(u)))
		break;                              /* ENOENT/EPERM/ESRCH: stop */
	printf("%.1f s  cpu %.0f%%  rss %.1f MB\n", u.wall_ns / 1e9,
	       u.cpu_permille / 10.0, u.rss_bytes / 1048576.0);
	if (u.state & PIDMON_ST_EXITED)
		break;
	sleep(1);
}
```

### Watch many children

One owner holds up to 64 windows at once, one per target. Start them all, then loop over them reading
and stopping. Releasing a window as soon as you have its numbers keeps slots free. Many windows
share one 10 ms sampler.

### A supervisor that survives its child's helpers

Make the supervisor a subreaper so that a grandchild orphaned by a short-lived intermediate is still
yours:

```c
prctl(PR_SET_CHILD_SUBREAPER, 1);
```

## Pitfalls

- **Start the window before the work.** CPU and memory the target used before the call are not in the
  window counters, and a breach is judged against the numbers at the time, so a target already over a
  limit is signalled at once.
- **Zero the struct.** An uninitialised `set` can name a limit that does not exist (`EINVAL`) or arm limits you did not mean, and uninitialised entries can have soft above hard (`EINVAL`).
- **Units differ.** CPU is in seconds, memory in bytes, `NOFILE` is a count. `utime_ns` etc. are
  nanoseconds.
- **`size` must match.** Pass `sizeof(struct ...)`. Passing `0` with a struct, or a size with `NULL`, is `EINVAL`.
- **Reading a pid you are not allowed to touch** is `EPERM`, and a pid that was never started is
  `ENOENT`, not `ESRCH`.
- **Reap and release.** `waitpid` frees the process, not the pidmon record. Always do the final read
  with `PIDMON_F_STOP`.
- **Warnings can be lost.** A target that blocks `SIGTERM`/`SIGXCPU` or ignores them is warned once and
  the kernel does not repeat. It will still be killed at the hard limit, which cannot be blocked.
- **Signals use the target's normal delivery.** A target stopped with `SIGSTOP` or in uninterruptible
  sleep still takes `SIGKILL` as soon as it can run.
- **Soft equal to hard** gives no warning. Leave a gap if you want one.
- **Fault counts depend on the system.** With transparent hugepages set to `always` the kernel
  batches page faults, so `minor_faults` is far smaller than the number of pages touched.
- **Memory numbers are for the process, not a cgroup.** Memory a target shares with others is counted
  in its `rss_file` or shared part.

## Compatibility and growing the structs

Both structs are versioned by size, as with `clone3`:

- `pidmon_limits` is 120 bytes today (`PIDMON_LIMITS_SIZE_VER0`). A future kernel may add fields at
  the end. A program built for the old size keeps working: the new fields default.
- A program built against a *newer* header, run on an *older* kernel, gets `E2BIG` if it sets a field
  the kernel does not know, and works if the extra tail is all zero. That is how a program can probe
  for a feature.
- `pidmon_usage` is 328 bytes today (`PIDMON_USAGE_SIZE_VER0`). A longer buffer gets its extra part
  zeroed, and `usage->size` says how much the kernel filled in.
- Always use `sizeof` of the struct you compiled against, never a number.

## Complete example

`docs/examples/guard.c` is a small supervisor: it forks a child that grows without end, opens a
window that warns at 24 MB of resident memory and kills at 48 MB, waits for the child, then reads and
releases the window.

Build it against the kernel's installed headers:

```sh
make -C linux O=build headers_install INSTALL_HDR_PATH=$PWD/hdr
gcc -O1 -Wall -isystem hdr/include -o guard docs/examples/guard.c
```

Run it on a kernel that has the calls (see the README for building and booting one). Its output on the
test kernel:

```
child: warned, but carrying on
child ended by signal 9 (Killed)
final report (target exited):
  window         310.5 ms, cpu 90.6 ms
  rss peak       48.5 MB, virtual peak 49.1 MB
  exceeded soft  RSS
  exceeded hard  RSS
  signal sent    Killed
```

The warning arrives at 24 MB, the child shrugs it off by handling `SIGTERM`, keeps growing, and is
killed at 48 MB. The `rss peak` is a little over the hard limit because the check runs every 10 ms.
Both breaches, and the strongest signal, are still reported after the child is gone.
