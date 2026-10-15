/* Phase 6 step 2: the stub syscall is wired through tables, headers and the build. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __NR_pidmon
#define __NR_pidmon 473
#endif

int main(void)
{
	long r = syscall(__NR_pidmon, 0, 0, NULL, 0);

	printf("stub: pidmon() = %ld (want 42)\n", r);
	return r == 42 ? 0 : 1;
}
