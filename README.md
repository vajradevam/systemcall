# pidmon: a process-monitoring system call for Linux (CS-G623)

A new system call in mainline Linux. Given a pid, it **reports that process's resource
utilization and enforces limits by taking an appropriate action**, but only if the pid is the
caller itself or one of its descendants (children, grandchildren, ...). For any other process it
refuses.

This repo holds the kernel source with our syscall, plus the scripts and tests we use to build
it, boot it in QEMU and check it. Everything below is what we decided, what is still open, and
how to work with the repo.

- Kernel base: mainline **Linux 7.3.0-rc7+**, commit `0fcb05b72` (14 Oct 2026), pinned in `BASE_COMMIT`
- Syscall name / number (proposed): **`pidmon`, 473** on x86-64 (472 was already `fchroot`)
- Status: harness and stub syscall done and passing; the real implementation is not written yet (see [Status](#status))

---

## 1. The problem in plain words

A parent process often starts children and wants to keep an eye on them: how much CPU have they
burned, how much memory do they hold, how many threads? If one runs away, the parent wants the
kernel to deal with it. Linux has pieces of this, but nothing that says "tell me about my
descendant and act on it in one call", so we add a syscall.

Two rules define it:

1. **Report and enforce.** Return resource usage for the target and take an action if a limit is exceeded.
2. **Authority comes from ancestry.** The target must be the caller or in the caller's descendant
   tree. A sibling, a parent, an unrelated process or pid 1 is refused (`EPERM`).

### Why not use what already exists?

| Existing tool | What it does | What it lacks |
|---|---|---|
| `getrusage(RUSAGE_CHILDREN)` | Usage of children that have **exited and been waited for** | Nothing about live descendants, no grandchildren view |
| `prlimit(pid, ...)` | Reads/sets the limits of a process | Reports no usage, takes no action |
| `/proc/<pid>/stat, status` | Read-only text for any visible process | Text parsing, racy, no ancestry rule, no action |
| Kernel rlimit enforcement | `RLIMIT_CPU` -> `SIGXCPU`/`SIGKILL` by timers; `RLIMIT_AS` -> `mmap` fails | **`RLIMIT_RSS` is never enforced**; event-driven only, nothing on demand |

So the new part is a tree-scoped report plus on-demand enforcement under the caller's control,
including for resources the kernel never enforces (RSS). With *pure rlimits* as the limit source
there is real overlap for CPU and address space; that is open question 2 below.

---

## 2. The part that is easy to get wrong: "descendant"

A process's parent can change. When a parent exits before its child, the child is **reparented**
(to another thread of the same group, else the nearest "subreaper", else pid 1). After that it is
no longer in the old ancestor's tree, and the old ancestor loses authority over it. Our check
must therefore use the *current* genealogy, not a remembered one.

Facts the implementation relies on:

- The user-visible pid is the **TGID** (thread group id). We work on `group_leader`, never on a single thread.
- Use **`real_parent`** (genealogy). `parent` can be changed by ptrace and must not be used.
- Walk `real_parent` upward from the target under `rcu_read_lock()`. If we reach the caller's
  thread group (`same_thread_group`), it is allowed; if we reach `init_task` first, return `EPERM`.
- Look the pid up with `find_task_by_vpid` (caller's pid namespace). Take references
  (`get_task_struct`, `get_pid`) before leaving RCU so a recycled pid cannot be hit (TOCTOU).
- Never sleep inside the RCU read section.
- Signals go out with `kill_pid(spid, sig, 1)` (`SEND_SIG_PRIV`), which skips the normal uid /
  `CAP_KILL` check. That is intentional: **ancestry is the authorization**. Delivery is asynchronous.

---

## 3. Proposed interface

```c
long pidmon(pid_t pid, unsigned int flags,
            struct pidmon_usage __user *uattr, size_t usize);
```

`include/uapi/linux/pidmon.h`:

```c
struct pidmon_usage {              /* PIDMON_USAGE_SIZE_VER0 = 48 bytes */
    __u64 utime_ns, stime_ns;      /* CPU time of the whole thread group */
    __u64 rss_bytes, vm_bytes;     /* resident / virtual memory */
    __u32 nr_threads;
    __u32 action;                  /* PIDMON_ACT_NONE / WARNED / KILLED */
    __u32 exceeded;                /* bitmask of which limits were over */
    __u32 __pad;
};
```

- `pid == 0` means "myself".
- `flags`: `PIDMON_F_NO_ENFORCE` (report only, dry run); unknown bits -> `EINVAL`. `PIDMON_F_VALID` is the mask of known bits.
- `usize` lets the struct grow in future versions (same pattern as `clone3` / `openat2`), copied with `copy_struct_to_user`.
- Errors: `EINVAL` (bad flags/size/pid), `ESRCH` (no such pid), `EPERM` (not self/descendant), `EFAULT` (bad user pointer).

The assignment only says "given the pid". Whether we may add the struct, size and flags is
question 1 below; a pid-only signature would force the report into the return value, which cannot
carry several numbers.

### Algorithm (what `kernel/pidmon.c` will do)

1. Validate `flags`, `usize`, `pid`.
2. Under RCU: find the target (`pid == 0` -> `current`), take its `group_leader`.
3. Walk `real_parent` upward; no path to the caller's thread group -> `EPERM`.
4. Take references (`get_task_struct`, `get_pid(task_tgid)`), leave RCU.
5. Collect: CPU via `thread_group_cputime_adjusted`, RSS via `get_mm_rss` (approximate, per-cpu batched), VM via `mm->total_vm` (`get_task_mm`/`mmput`), `get_nr_threads`.
6. Judge against limits (CPU, address space, RSS). Soft limit exceeded -> warning signal; hard limit -> `SIGKILL`.
7. `copy_struct_to_user` first, then `kill_pid(...)` unless `NO_ENFORCE`. (Report even if we kill.)
8. `put_pid`, `put_task_struct`.

### Files we change in the kernel (and why)

| File | Change |
|---|---|
| `linux/kernel/pidmon.c` (new) | The syscall (currently a stub returning 42) |
| `linux/include/uapi/linux/pidmon.h` (new) | User-visible struct and flags; also fixes "struct declared inside parameter list" |
| `linux/kernel/Makefile` | `obj-y += pidmon.o` |
| `linux/include/linux/syscalls.h` | Prototype `sys_pidmon` |
| `linux/arch/x86/entry/syscalls/syscall_64.tbl` | `473 common pidmon sys_pidmon` (generates the dispatch table) |
| `linux/arch/x86/entry/syscalls/syscall_32.tbl` | Same for 32-bit |
| `linux/include/uapi/asm-generic/unistd.h` | `__NR_pidmon 473`, `__NR_syscalls` bumped (covers arm64 and others) |
| `linux/scripts/syscall.tbl` | Generic table entry |

`./scripts/60-mkpatch.sh` produces `pidmon.patch`: exactly these changes against the pinned base.

---

## 4. Questions for the professor (not yet answered)

The behaviour that is easy to get wrong is exactly what the statement leaves open. Until these are
answered our choices are assumptions.

1. **Interface.** May the signature add an output-struct pointer, a size argument and a flags argument (as `clone3` / `openat2` do), or must it be pid-only with the result in the return value?
   *Why size and flags:* `usize` lets the struct grow without breaking old programs; `flags` is the standard way to add options later (we use it for the dry-run mode). Both are cheap now and impossible to retrofit.
2. **Source of the limits.** The kernel already enforces `RLIMIT_CPU` and `RLIMIT_AS` by itself. Should we use the target's existing rlimits, or do you want a separate limit mechanism (fixed thresholds, or limits set through another call)? If separate, we need somewhere to store them (e.g. in `signal_struct`) and a way to set them, which probably changes the interface.
3. **Which resources count.** CPU time, resident memory, virtual memory, threads? Anything else (open files, I/O)?
4. **What "appropriate action" means.** Warn (`SIGXCPU`/`SIGTERM`) at the soft limit and `SIGKILL` at the hard limit? Report only? Is a dry-run flag acceptable?
5. **Name and deliverables.** Is the name ours to choose (we propose `pidmon`)? What do you want handed in: a patch against mainline, a source tree, a demo on another machine, a report?

---

## 5. Repository layout

```
systemcall/
├── linux/                     kernel source: mainline 0fcb05b72 + our changes (a plain folder, not a nested repo)
├── scripts/                   build / boot / test scripts
├── tests/                     user-space tests, one C file each (NN_name.c)
├── kconfig/                   tiny.config, debug.config (kernel config fragments)
├── pidmon_syscall_guide.tex   LaTeX write-up (design, background, implementation steps, tests)
├── pidmon_syscall_guide.pdf
├── BASE_COMMIT                mainline commit the kernel is based on
└── build-*/ hdr/ rootfs/ out/ generated, git-ignored
```

Why one repo with the kernel inside: we share everything with a single clone, pull and push.
Why the kernel is a plain folder and not a fork: a fork needs a second repo and a second push for
every change, and the history of the kernel is irrelevant to us. The base is pinned in
`BASE_COMMIT` so both of us build exactly the same kernel and the final patch can be rebuilt.

---

## 6. How we build and test: QEMU + busybox

Compiling and rebooting a real machine for every edit would be slow and risky. Instead:

- A **minimal kernel** (`tinyconfig` + `kconfig/tiny.config`, only what is needed to boot, mount `/proc` and run a static binary) builds in a few minutes the first time and seconds afterwards (ccache, incremental).
- A tiny **initramfs** with a static **busybox** and our compiled tests is the whole userland.
- **QEMU** (with KVM when available) boots it and the guest `/init` runs every `/tests/*`, greps `dmesg` for `BUG:`, `WARNING:`, `Oops`, `KASAN`, `lockdep`, prints `PIDMON_EXIT=<n>` and powers off with `reboot -f` (tinyconfig has no ACPI, so `poweroff` would not stop QEMU).
- The host script turns that into one line: **`RESULT: PASS` / `FAIL` / `TIMEOUT`**.

Two profiles share the scripts: `PROFILE=tiny` (default, fast) and `PROFILE=debug` (KASAN, lockdep, atomic-sleep and list checks, `panic_on_oops`), each in its own build directory.

### Will it work on "another OS"?

The syscall lives in the kernel source, so any kernel built from this source has it. For the
demo, build the same source with the target machine's config (e.g. its `/boot/config-*` plus
`make olddefconfig`) and boot that kernel on another **Linux** install or VM. It is not portable to
Windows, macOS or BSD. Things to remember:

- Run the test suite once with a full distro-style config in QEMU too; preemption, cgroups and RSS accounting differ from tinyconfig.
- glibc has no wrapper: tests call `syscall(473, ...)` and compile against the headers from `make headers_install` (`hdr/`), not the host's.
- ARM64 is covered by the generic `unistd.h` change; x86 needs the `.tbl` entries (done).
- Add a test that drops to an unprivileged uid, since "ancestry is the authorization" matters most there.

---

## 7. Setup and daily use

**Host tools** (`./scripts/00-deps.sh` checks them): gcc, make, bc, flex, bison, cpio, gzip, file,
qemu-system-x86_64, and a **static busybox** (`pacman -S busybox`; on Debian/Ubuntu
`apt install busybox-static`; or set `BUSYBOX=/path`). `ccache` and `/dev/kvm` are optional but make it much faster.

```bash
git clone --depth 1 https://github.com/vajradevam/systemcall.git
cd systemcall
./scripts/00-deps.sh
./scripts/run-all.sh          # config (once) + build + initramfs + QEMU + result
```

| Command | What it does |
|---|---|
| `./scripts/run-all.sh` | Whole loop |
| `PROFILE=debug ./scripts/run-all.sh` | Same with KASAN / lockdep kernel |
| `./scripts/run-all.sh --rootfs-only` | Only tests changed: skip the kernel rebuild |
| `./scripts/50-run.sh --gdb` | Boot paused, gdb on `:1234` |
| `RUN_TIMEOUT=300 ./scripts/50-run.sh` | Longer timeout for slow machines |
| `./scripts/60-mkpatch.sh` | Write `pidmon.patch` (our kernel changes vs `BASE_COMMIT`) |

The scripts, in order: `00-deps` (check tools), `20-config` (tinyconfig + fragments, warns about
dropped options), `30-build` (bzImage + `headers_install`), `40-rootfs` (busybox + tests ->
initramfs), `50-run` (QEMU), `run-all` (all of it).

**Working together:** `git pull --rebase`, edit, `./scripts/run-all.sh`, `git add -A`, `git commit`, `git push`.
To avoid conflicts, one person owns `linux/kernel/pidmon.c` and the kernel side; the other owns
`tests/` and the docs. Commit often and pull before starting.

---

## 8. Status

Done:
- [x] Harness: minimal config, static busybox initramfs, QEMU with KVM, PASS/FAIL/TIMEOUT detection, debug profile
- [x] Stub syscall 473 wired through tables, prototype, UAPI header and build; user space sees `pidmon() = 42`
- [x] Verified end to end from a fresh clone of this repo

Next, in order:
1. Verify helper signatures in this tree (`thread_group_cputime_adjusted`, `task_rlimit`, `copy_struct_to_user`, `kill_pid`, `get_mm_rss`).
2. Authorization only (ancestry walk) plus relationship tests.
3. Usage collection, cross-checked against `/proc`.
4. Limit judging with `NO_ENFORCE`.
5. Signalling.
6. Edge cases: reparenting, multithreaded targets, zombies, PID namespaces.
7. Run everything under `PROFILE=debug` with a clean `dmesg`.
8. Stress test (fork-chain loop).
9. Polish: `checkpatch.pl --strict`, selftest, docs, patch.
10. Rebuild the LaTeX guide and PDF with the final results.

### Test matrix (to be written in `tests/`)

Each case forks its own process tree so a kill cannot take down the test runner.

| Case | Expected |
|---|---|
| self (`pid` = own pid, and `pid` = 0) | OK, usage returned |
| child, grandchild | OK |
| parent, sibling, pid 1 | `EPERM` |
| nonexistent pid | `ESRCH` |
| bad flags / bad `usize` / negative pid | `EINVAL` |
| bad user pointer | `EFAULT` |
| CPU hard limit exceeded | target killed |
| RSS soft limit exceeded | warned |
| `NO_ENFORCE` | reported, never signalled |
| orphan reparented to init | now `EPERM` for the old ancestor |
| multithreaded target, zombie target | correct totals / sensible result |
| process in a child PID namespace (`CLONE_NEWPID`) | resolved in the caller's namespace |
| unprivileged caller on its own child | OK |

---

## 9. Problems met so far (and fixes)

| Problem | Cause | Fix |
|---|---|---|
| `struct pidmon_usage declared inside parameter list` | struct not visible to `syscalls.h` | `include/uapi/linux/pidmon.h`, included by `kernel/pidmon.c` |
| `RESULT: TIMEOUT` with no output in a real terminal | `timeout` runs QEMU in a background process group; reading the tty stops it (SIGTTIN) | `</dev/null` in `50-run.sh` |
| "busybox is not static" for a static busybox | `ldd` exits non-zero on static files under `set -o pipefail` | check with `file -L ... | grep 'statically linked'` |
| QEMU never exits after the tests | tinyconfig has no ACPI | `reboot -f` + `-no-reboot` + `reboot=k` |
| Struct size said 40, really 48 | 4 x 8 + 4 x 4 | `PIDMON_USAGE_SIZE_VER0 = 48` |
| Four kernel files missing after the push | ignore rule `hdr/` matched kernel folders named `hdr` | anchored the ignore rules to the repo root (`/hdr/`) |

---

## 10. Further reading

- `pidmon_syscall_guide.pdf` / `.tex`: design, background, step-by-step implementation, test plan
- `linux/Documentation/process/adding-syscalls.rst`: the kernel's own checklist
- `linux/kernel/sys.c` (`getrusage`, `prlimit`), `linux/kernel/signal.c` (`kill_pid_info`): code we model ours on
