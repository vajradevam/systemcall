# pidmon syscall: CS-G623

One repo: our scripts next to the kernel source.

    systemcall/
    ├── linux/       kernel source (mainline + our syscall)       BASE_COMMIT = mainline commit it is based on
    ├── scripts/     build / boot / test scripts
    ├── tests/       user-space tests (*.c)
    ├── kconfig/     tiny.config, debug.config
    ├── pidmon_syscall_guide.tex/pdf
    └── build-*/ hdr/ rootfs/ out/   generated, git-ignored

## Setup (once)

    git clone --depth 1 https://github.com/vajradevam/systemcall.git && cd systemcall
    ./scripts/00-deps.sh        # lists missing host tools (needs a *static* busybox)
    ./scripts/run-all.sh        # config + build + initramfs + QEMU

## Commands

    ./scripts/run-all.sh                 # full loop             -> RESULT: PASS / FAIL / TIMEOUT
    PROFILE=debug ./scripts/run-all.sh   # KASAN + lockdep build
    ./scripts/run-all.sh --rootfs-only   # only tests changed: skip the kernel rebuild
    ./scripts/50-run.sh --gdb            # boot paused, gdb on :1234
    ./scripts/60-mkpatch.sh              # writes pidmon.patch = our kernel changes vs. BASE_COMMIT

## Daily workflow

    git pull --rebase ; edit ; ./scripts/run-all.sh ; git add -A ; git commit ; git push

Suggested split: one person owns `linux/kernel/pidmon.c`, the other `tests/` and docs.
