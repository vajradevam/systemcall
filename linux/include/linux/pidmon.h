/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PIDMON_H
#define _LINUX_PIDMON_H

#include <linux/compiler.h>

struct task_struct;

/* number of live monitor records; zero almost always */
extern int pidmon_nrecs;

/* called by the last thread of a thread group as it exits, before exit_mm() */
void pidmon_exit_group(struct task_struct *tsk);

static inline void pidmon_exit(struct task_struct *tsk)
{
	if (unlikely(READ_ONCE(pidmon_nrecs)))
		pidmon_exit_group(tsk);
}

#endif /* _LINUX_PIDMON_H */
