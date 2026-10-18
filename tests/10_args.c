/* Argument validation, call ordering, errno contract and the per-owner cap. */
#include "common.h"

int main(void)
{
	struct pidmon_usage u;
	unsigned char big[4096];
	pid_t kids[PIDMON_MAX_RECS_FOR_TEST + 2];
	int i, r, n_ok = 0;

	test_begin("10_args");

	/* struct layout is part of the ABI */
	CHECK(sizeof(u) == PIDMON_USAGE_SIZE_VER0, "sizeof=%zu", sizeof(u));

	/* bad arguments */
	EXPECT_ERR("start flags", pm_start(0, 1), EINVAL);
	EXPECT_ERR("start neg pid", pm_start(-1, 0), EINVAL);
	EXPECT_ERR("read neg pid", pm_read(-1, 0, &u), EINVAL);
	EXPECT_ERR("read bad flags", pm_read(0, 0x80, &u), EINVAL);
	EXPECT_ERR("read usize too small",
		   (int)(syscall(__NR_pidmon_read, 0, 0, &u, 8) < 0 ? errno : 0), EINVAL);
	EXPECT_ERR("read usize too big",
		   (int)(syscall(__NR_pidmon_read, 0, 0, &u, 1 << 20) < 0 ? errno : 0), E2BIG);

	/* nothing was started yet */
	EXPECT_ERR("read before start", pm_read(0, 0, &u), ENOENT);
	EXPECT_ERR("start nonexistent pid", pm_start(999999, 0), ESRCH);
	EXPECT_ERR("read nonexistent pid", pm_read(999999, 0, &u), ENOENT);

	/* a bad user pointer fails cleanly and leaves the monitor intact */
	EXPECT_ERR("start self", pm_start(0, 0), 0);
	r = syscall(__NR_pidmon_read, 0, 0, (void *)1, sizeof(u)) < 0 ? errno : 0;
	EXPECT_ERR("read bad pointer", r, EFAULT);
	EXPECT_ERR("start again", pm_start(0, 0), EBUSY);
	EXPECT_ERR("start again by own pid", pm_start(getpid(), 0), EBUSY);

	/* a normal read fills in the size and a sane window */
	EXPECT_ERR("read self", pm_read(0, 0, &u), 0);
	CHECK(u.size == sizeof(u), "size=%u", u.size);
	CHECK(u.wall_ns > 0, "wall_ns=%llu", (unsigned long long)u.wall_ns);
	CHECK(u.nr_threads == 1, "nr_threads=%llu", (unsigned long long)u.nr_threads);
	CHECK(u.exceeded_soft == 0 && u.exceeded_hard == 0 && u.signal_sent == 0,
	      "default rlimits must not trip: soft=%x hard=%x sig=%u",
	      u.exceeded_soft, u.exceeded_hard, u.signal_sent);
	CHECK(!(u.state & PIDMON_ST_EXITED), "state=%x", u.state);

	/* a bigger buffer than the kernel struct: the tail must be zeroed */
	memset(big, 0xAA, sizeof(big));
	r = syscall(__NR_pidmon_read, 0, 0, big, sizeof(big)) < 0 ? errno : 0;
	EXPECT_ERR("read big buffer", r, 0);
	for (i = sizeof(u); i < (int)sizeof(big); i++)
		if (big[i]) {
			CHECK(0, "tail byte %d not zeroed", i);
			break;
		}

	/* STOP releases the monitor */
	EXPECT_ERR("read STOP", pm_read(0, PIDMON_F_STOP, &u), 0);
	EXPECT_ERR("read after STOP", pm_read(0, 0, &u), ENOENT);
	EXPECT_ERR("restart after STOP", pm_start(getpid(), 0), 0);
	EXPECT_ERR("STOP again", pm_read(getpid(), PIDMON_F_STOP, &u), 0);

	/* per-owner cap */
	for (i = 0; i < PIDMON_MAX_RECS_FOR_TEST + 2; i++) {
		int fd;

		kids[i] = spawn(sleeper, NULL, &fd);
		get_byte(fd, 2000);
		close(fd);
		r = pm_start(kids[i], 0);
		if (r == 0)
			n_ok++;
		else
			EXPECT_ERR("start over the cap", r, ENOSPC);
	}
	CHECK(n_ok == PIDMON_MAX_RECS_FOR_TEST, "%d monitors accepted, want %d",
	      n_ok, PIDMON_MAX_RECS_FOR_TEST);
	for (i = 0; i < PIDMON_MAX_RECS_FOR_TEST + 2; i++)
		kill_reap(kids[i]);

	return test_end();
}
