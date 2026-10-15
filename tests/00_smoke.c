/* Phase 6 step 1: proves the boot -> init -> static binary -> console path works. */
#include <stdio.h>
#include <sys/utsname.h>

int main(void)
{
	struct utsname u;

	if (uname(&u))
		return 1;
	printf("smoke: %s %s %s\n", u.sysname, u.release, u.machine);
	return 0;
}
