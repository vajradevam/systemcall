/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_PIDMON_H
#define _UAPI_LINUX_PIDMON_H

#include <linux/types.h>

#define PIDMON_F_NO_ENFORCE	(1U << 0)	/* report only, take no action */
#define PIDMON_F_VALID		(PIDMON_F_NO_ENFORCE)

#define PIDMON_ACT_NONE		0
#define PIDMON_ACT_WARNED	1	/* soft limit exceeded */
#define PIDMON_ACT_KILLED	2	/* hard limit exceeded */

struct pidmon_usage {
	__u64 utime_ns;		/* user CPU time, whole thread group */
	__u64 stime_ns;		/* system CPU time */
	__u64 rss_bytes;
	__u64 vm_bytes;
	__u32 nr_threads;
	__u32 action;		/* PIDMON_ACT_* taken (or that would be taken) */
	__u32 exceeded;		/* bitmask of RLIMIT_* numbers that tripped */
	__u32 __pad;
	/* new fields go below this line */
};
#define PIDMON_USAGE_SIZE_VER0	48	/* sizeof(struct pidmon_usage) at first release */

#endif /* _UAPI_LINUX_PIDMON_H */
