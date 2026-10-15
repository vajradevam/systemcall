// SPDX-License-Identifier: GPL-2.0
/* STUB: harness check only, replaced by the real implementation. */
#include <linux/syscalls.h>
#include <uapi/linux/pidmon.h>

SYSCALL_DEFINE4(pidmon, pid_t, pid, unsigned int, flags,
		struct pidmon_usage __user *, uattr, size_t, usize)
{
	return 42;
}
