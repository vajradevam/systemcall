/* The memory fields follow the target's address space: mlock shows in locked_vm, private
 * mappings in data_vm and vm_bytes, and freeing memory lowers the gauges but not the peaks. */
#include "common.h"

#define MB (1024ULL * 1024)

static void worker(int in, int out)
{
	char *a, *b;

	put_byte(out, 'r');
	get_byte(in, 10000);

	a = touch_mb(24);		/* 24 MB private + resident */
	put_byte(out, '1');
	get_byte(in, 10000);

	b = mmap(NULL, 8 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (b == MAP_FAILED || mlock(b, 8 << 20))
		die("mlock");		/* 8 MB locked, hence resident */
	put_byte(out, '2');
	get_byte(in, 10000);

	munlock(b, 8 << 20);
	munmap(a, 24 << 20);
	munmap(b, 8 << 20);
	put_byte(out, '3');
	for (;;)
		pause();
}

int main(void)
{
	struct pidmon_usage u0, u1, u2, u3;
	int to, from;
	pid_t c;

	test_begin("35_memory");

	c = spawn(worker, &to, &from);
	CHECK(get_byte(from, 10000) == 'r', "worker not ready");
	EXPECT_ERR("start", pm_start(c, 0), 0);
	EXPECT_ERR("read base", pm_read(c, 0, &u0), 0);
	CHECK(u0.stack_vm > 0, "stack_vm=%llu", (unsigned long long)u0.stack_vm);
	CHECK(u0.rss_bytes == u0.rss_anon + u0.rss_file ||
	      u0.rss_bytes >= u0.rss_anon + u0.rss_file, "rss split: %llu = %llu + %llu",
	      (unsigned long long)u0.rss_bytes, (unsigned long long)u0.rss_anon,
	      (unsigned long long)u0.rss_file);

	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == '1', "step 1");
	EXPECT_ERR("read 24 MB", pm_read(c, 0, &u1), 0);
	CHECK(u1.vm_bytes >= u0.vm_bytes + 24 * MB, "vm_bytes %llu -> %llu",
	      (unsigned long long)u0.vm_bytes, (unsigned long long)u1.vm_bytes);
	CHECK(u1.data_vm >= u0.data_vm + 24 * MB, "data_vm %llu -> %llu",
	      (unsigned long long)u0.data_vm, (unsigned long long)u1.data_vm);
	CHECK(u1.rss_anon >= u0.rss_anon + 23 * MB, "rss_anon %llu -> %llu",
	      (unsigned long long)u0.rss_anon, (unsigned long long)u1.rss_anon);
	CHECK(u1.locked_vm == u0.locked_vm, "locked_vm moved: %llu", (unsigned long long)u1.locked_vm);
	CHECK(u1.rss_peak >= u1.rss_bytes && u1.vm_peak >= u1.vm_bytes, "peak below gauge");

	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == '2', "step 2");
	EXPECT_ERR("read mlock", pm_read(c, 0, &u2), 0);
	CHECK(u2.locked_vm >= u1.locked_vm + 8 * MB, "locked_vm %llu -> %llu",
	      (unsigned long long)u1.locked_vm, (unsigned long long)u2.locked_vm);
	CHECK(u2.vm_bytes >= u1.vm_bytes + 8 * MB, "vm_bytes %llu -> %llu",
	      (unsigned long long)u1.vm_bytes, (unsigned long long)u2.vm_bytes);
	CHECK(proc_status(c, "VmLck:") * 1024ULL == u2.locked_vm, "VmLck=%ld kB vs %llu",
	      proc_status(c, "VmLck:"), (unsigned long long)u2.locked_vm);

	put_byte(to, 'g');
	CHECK(get_byte(from, 10000) == '3', "step 3");
	EXPECT_ERR("read freed", pm_read(c, 0, &u3), 0);
	CHECK(u3.vm_bytes + 30 * MB <= u2.vm_bytes, "vm_bytes did not drop: %llu -> %llu",
	      (unsigned long long)u2.vm_bytes, (unsigned long long)u3.vm_bytes);
	CHECK(u3.rss_bytes + 30 * MB <= u2.rss_bytes, "rss_bytes did not drop: %llu -> %llu",
	      (unsigned long long)u2.rss_bytes, (unsigned long long)u3.rss_bytes);
	CHECK(u3.locked_vm == u0.locked_vm, "locked_vm=%llu", (unsigned long long)u3.locked_vm);
	/* the kernel's hiwater marks come from approximate per-CPU counters: allow 1 MB of slack */
	CHECK(u3.rss_peak + MB >= u2.rss_bytes, "rss_peak %llu forgot the high point %llu",
	      (unsigned long long)u3.rss_peak, (unsigned long long)u2.rss_bytes);
	CHECK(u3.vm_peak + MB >= u2.vm_bytes, "vm_peak %llu forgot the high point %llu",
	      (unsigned long long)u3.vm_peak, (unsigned long long)u2.vm_bytes);

	kill_reap(c);
	return test_end();
}
