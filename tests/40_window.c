/* Counters and peaks cover the window since pidmon_start(), not the process's whole life. */
#include "common.h"

#define MB (1024ULL * 1024)

static void worker(int in, int out)
{
	void *p = touch_mb(64);	/* big peak before the window */

	burn_ms(300);		/* CPU before the window */
	munmap(p, 64 << 20);
	put_byte(out, 'r');
	get_byte(in, 10000);

	touch_mb(8);		/* the window's own peak */
	burn_ms(50);
	put_byte(out, 'd');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u;
	int to, from;
	pid_t c;

	test_begin("40_window");

	c = spawn(worker, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "worker not ready");

	EXPECT_ERR("start", pm_start(c, 0), 0);
	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == 'd', "worker did not finish");
	EXPECT_ERR("read", pm_read(c, 0, &u), 0);

	CHECK(u.rss_peak >= 8 * MB && u.rss_peak < 40 * MB,
	      "rss_peak=%llu: must reflect the 8 MB window, not the earlier 64 MB",
	      (unsigned long long)u.rss_peak);
	CHECK(u.vm_peak < 60 * MB, "vm_peak=%llu", (unsigned long long)u.vm_peak);
	CHECK(u.utime_ns + u.stime_ns >= 40000000ULL &&
	      u.utime_ns + u.stime_ns < 250000000ULL,
	      "window cpu=%llu ns: must exclude the 300 ms burnt before start",
	      (unsigned long long)(u.utime_ns + u.stime_ns));
	CHECK(u.cpu_total_ns >= u.utime_ns + u.stime_ns + 250000000ULL,
	      "cpu_total=%llu must include the pre-window burn", (unsigned long long)u.cpu_total_ns);
	CHECK(u.minor_faults >= 1900 && u.minor_faults < 6000, "minor_faults=%llu",
	      (unsigned long long)u.minor_faults);

	/* the kernel's own VmHWM was restarted by start: same number as ours */
	CHECK(proc_status(c, "VmHWM:") * 1024ULL < 40 * MB, "VmHWM=%ld kB", proc_status(c, "VmHWM:"));

	kill_reap(c);
	return test_end();
}
