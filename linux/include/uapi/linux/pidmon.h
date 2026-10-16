/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_PIDMON_H
#define _UAPI_LINUX_PIDMON_H

#include <linux/types.h>

/*
 * pidmon: monitor a process in the caller's descendant tree.
 *
 *   pidmon_start(pid, 0)                      start a monitoring window
 *   pidmon_read(pid, flags, &usage, size)     aggregate, judge, enforce
 *
 * "pid" is a thread-group id as seen in the caller's pid namespace; 0 means
 * the caller itself.  The target must be the caller or a (grand)child of it,
 * judged by the genealogy at the time of each call.
 */

/* pidmon_read() flags */
#define PIDMON_F_NO_ENFORCE	(1U << 0)	/* report only, never signal */
#define PIDMON_F_STOP		(1U << 1)	/* free the monitor after reading */
#define PIDMON_F_READ_VALID	(PIDMON_F_NO_ENFORCE | PIDMON_F_STOP)

/* pidmon_usage.state */
#define PIDMON_ST_EXITED		(1U << 0) /* target is gone; these are its final numbers */
#define PIDMON_ST_ALREADY_SIGNALLED	(1U << 1) /* this severity was already signalled earlier */

/*
 * The limits that are judged, in the order used by pidmon_usage.lim[] and by
 * the exceeded_soft / exceeded_hard bitmasks (bit n <=> limit n).  Each one is
 * the target's RLIMIT_* of the same name, in the unit of that rlimit:
 * CPU in seconds, NOFILE as a count, the rest in bytes.  ~0 means unlimited.
 */
#define PIDMON_LIM_CPU		0	/* RLIMIT_CPU, compared with total CPU time */
#define PIDMON_LIM_AS		1	/* RLIMIT_AS, virtual size */
#define PIDMON_LIM_RSS		2	/* RLIMIT_RSS, resident size (never kernel-enforced) */
#define PIDMON_LIM_DATA		3	/* RLIMIT_DATA */
#define PIDMON_LIM_STACK	4	/* RLIMIT_STACK, main-thread stack mapping */
#define PIDMON_LIM_MEMLOCK	5	/* RLIMIT_MEMLOCK */
#define PIDMON_LIM_NOFILE	6	/* RLIMIT_NOFILE, open file descriptors */
#define PIDMON_NLIM		7

struct pidmon_rlim {
	__aligned_u64 soft;
	__aligned_u64 hard;
};

/*
 * Counters are reported as the change since pidmon_start() ("window");
 * gauges as the value now, with peaks measured since pidmon_start().
 * Once the target has exited, everything is frozen at its exit
 * (PIDMON_ST_EXITED).
 */
struct pidmon_usage {
	/* header */
	__u32 size;		/* bytes of this struct the kernel filled in */
	__u32 state;		/* PIDMON_ST_* */
	__u32 exceeded_soft;	/* bitmask of PIDMON_LIM_* above their soft limit */
	__u32 exceeded_hard;	/* bitmask of PIDMON_LIM_* above their hard limit */
	__u32 signal_sent;	/* signal sent by this call, or 0 */
	__u32 __pad;

	/* time */
	__aligned_u64 wall_ns;		/* length of the window */

	/* CPU and scheduling (whole thread group) */
	__aligned_u64 utime_ns;		/* user CPU time in the window */
	__aligned_u64 stime_ns;		/* system CPU time in the window */
	__aligned_u64 cpu_total_ns;	/* user + system CPU since the process began */
	__aligned_u64 cpu_permille;	/* average CPU use in the window, 1000 = one full CPU */
	__aligned_u64 vol_ctxsw;	/* voluntary context switches in the window */
	__aligned_u64 invol_ctxsw;	/* involuntary context switches in the window */

	/* page faults in the window */
	__aligned_u64 minor_faults;
	__aligned_u64 major_faults;

	/* I/O in the window; zero when the kernel lacks the accounting option */
	__aligned_u64 rchar;		/* bytes asked of read-type syscalls */
	__aligned_u64 wchar;		/* bytes given to write-type syscalls */
	__aligned_u64 read_bytes;	/* bytes fetched from storage */
	__aligned_u64 write_bytes;	/* bytes sent to storage */

	/* memory, in bytes */
	__aligned_u64 rss_bytes;	/* resident now = rss_anon + rss_file + shared memory */
	__aligned_u64 rss_anon;
	__aligned_u64 rss_file;
	__aligned_u64 rss_peak;		/* peak resident since pidmon_start() */
	__aligned_u64 vm_bytes;		/* virtual size now */
	__aligned_u64 vm_peak;		/* peak virtual size since pidmon_start() */
	__aligned_u64 data_vm;		/* private writable mappings */
	__aligned_u64 stack_vm;		/* main-thread stack mapping (other stacks count as data) */
	__aligned_u64 locked_vm;	/* mlock()ed */

	/* process */
	__aligned_u64 nr_threads;
	__aligned_u64 nr_open_fds;

	/* limits at the time of the call */
	struct pidmon_rlim lim[PIDMON_NLIM];

	/* new fields go below this line */
};

#define PIDMON_USAGE_SIZE_VER0	328	/* sizeof(struct pidmon_usage) at first release */

#endif /* _UAPI_LINUX_PIDMON_H */
