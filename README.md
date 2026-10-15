# pidmon syscall: CS-G623

Two repos, kept separate:

| what | where |
|---|---|
| this repo: scripts, tests, kconfig, docs | `vajradevam/systemcall` |
| kernel with our syscall (fork of mainline, branch `pidmon`) | `vajradevam/linux-pidmon` -> cloned into `linux/` |

    systemcall/
    ├── scripts/   build / boot / test scripts      ├── linux/      kernel clone (own git repo, ignored here)
    ├── tests/     user-space tests (*.c)           ├── build-*/ hdr/ rootfs/ out/   generated, ignored
    ├── kconfig/   tiny.config, debug.config        └── pidmon_syscall_guide.tex/pdf
    └── BASE_COMMIT  mainline commit the kernel is based on

## Setup (once)

    git clone https://github.com/vajradevam/systemcall.git && cd systemcall
    ./scripts/00-deps.sh        # lists missing host tools (needs a *static* busybox)
    ./scripts/run-all.sh        # clones linux/ if needed, config + build + initramfs + QEMU

## Commands

    ./scripts/run-all.sh                 # full loop             -> RESULT: PASS / FAIL / TIMEOUT
    PROFILE=debug ./scripts/run-all.sh   # KASAN + lockdep build
    ./scripts/run-all.sh --rootfs-only   # only tests changed: skip the kernel rebuild
    ./scripts/50-run.sh --gdb            # boot paused, gdb on :1234

## Daily workflow (two commits, two repos)

    # kernel change                          # harness/test change
    cd linux; git commit; git push           cd ..; git commit; git push
    (other person) cd linux; git pull        (other person) git pull

Suggested split: one person owns `linux/kernel/pidmon.c`, the other `tests/` and docs.
Do NOT use GitHub "Sync fork" on linux-pidmon (it would move the kernel base).

## Handing in

    cd linux && git format-patch $(cat ../BASE_COMMIT) -o ../patches      # or: git diff $(cat ../BASE_COMMIT) > ../pidmon.patch
