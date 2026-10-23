/* rchar / wchar count the bytes handed to read- and write-type syscalls in the window only. */
#include "common.h"

#define KB 1024ULL

static void worker(int in, int out)
{
	char buf[4096];
	int z = open("/dev/zero", O_RDONLY), n = open("/dev/null", O_WRONLY);
	int i;

	if (z < 0 || n < 0)
		die("open");
	/* before the window: 64 KB read, 32 KB written */
	for (i = 0; i < 16; i++)
		if (read(z, buf, sizeof(buf)) != sizeof(buf))
			die("read");
	for (i = 0; i < 8; i++)
		if (write(n, buf, sizeof(buf)) != sizeof(buf))
			die("write");
	put_byte(out, 'r');
	get_byte(in, 10000);

	/* inside the window: 256 KB read, 128 KB written */
	for (i = 0; i < 64; i++)
		if (read(z, buf, sizeof(buf)) != sizeof(buf))
			die("read");
	for (i = 0; i < 32; i++)
		if (write(n, buf, sizeof(buf)) != sizeof(buf))
			die("write");
	put_byte(out, 'd');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u;
	int to, from;
	pid_t c;

	test_begin("45_io");

	c = spawn(worker, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "worker not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == 'd', "worker did not finish");
	EXPECT_ERR("read", pm_read(c, PIDMON_F_NO_ENFORCE, &u), 0);

	/* a little slack for the handshake pipe bytes, none for the big transfers */
	CHECK(u.rchar >= 256 * KB && u.rchar < 256 * KB + 4 * KB, "rchar=%llu want ~256 KB",
	      (unsigned long long)u.rchar);
	CHECK(u.wchar >= 128 * KB && u.wchar < 128 * KB + 4 * KB, "wchar=%llu want ~128 KB",
	      (unsigned long long)u.wchar);
	CHECK(u.read_bytes == 0 && u.write_bytes == 0, "no storage I/O expected: %llu / %llu",
	      (unsigned long long)u.read_bytes, (unsigned long long)u.write_bytes);

	/* quiet from now on: the counters must stand still */
	{
		struct pidmon_usage v;

		msleep(100);
		EXPECT_ERR("read again", pm_read(c, PIDMON_F_NO_ENFORCE, &v), 0);
		CHECK(v.rchar == u.rchar && v.wchar == u.wchar, "counters moved while idle");
	}

	/* the kernel's own /proc/<pid>/io shows the whole life, so it must be the larger */
	{
		char path[64], line[128];
		unsigned long long rc = 0;
		FILE *f;

		snprintf(path, sizeof(path), "/proc/%d/io", c);
		f = fopen(path, "r");
		if (f) {
			while (fgets(line, sizeof(line), f))
				sscanf(line, "rchar: %llu", &rc);
			fclose(f);
			CHECK(rc >= u.rchar + 64 * KB, "proc rchar=%llu vs window %llu", rc,
			      (unsigned long long)u.rchar);
		}
	}

	kill_reap(c);
	return test_end();
}
