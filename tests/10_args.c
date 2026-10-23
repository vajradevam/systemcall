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
	EXPECT_ERR("start flags", pm_start(0, 0x80), EINVAL);
	EXPECT_ERR("start with a read flag", pm_start(0, PIDMON_F_STOP), EINVAL);
	EXPECT_ERR("start neg pid", pm_start(-1, 0), EINVAL);
	EXPECT_ERR("read neg pid", pm_read(-1, 0, &u), EINVAL);
	EXPECT_ERR("read bad flags", pm_read(0, 0x80, &u), EINVAL);
	EXPECT_ERR("read with a start flag", pm_read(0, PIDMON_F_NO_ENFORCE, &u), EINVAL);
	EXPECT_ERR("read usize too small",
		   (int)(syscall(__NR_pidmon_read, 0, 0, &u, 8) < 0 ? errno : 0), EINVAL);
	EXPECT_ERR("read usize too big",
		   (int)(syscall(__NR_pidmon_read, 0, 0, &u, 1 << 20) < 0 ? errno : 0), E2BIG);

	/* nothing was started yet */
	EXPECT_ERR("read before start", pm_read(0, 0, &u), ENOENT);
	EXPECT_ERR("start nonexistent pid", pm_start(999999, 0), ESRCH);
	EXPECT_ERR("read nonexistent pid", pm_read(999999, 0, &u), ENOENT);

	/* the limits struct is validated before anything is armed */
	{
		struct pidmon_limits lim;
		unsigned char fat[512];
		long rc;

		memset(&lim, 0, sizeof(lim));
		lim_set(&lim, PIDMON_LIM_RSS, 10, 5);
		EXPECT_ERR("soft above hard", pm_start_lim(0, 0, &lim), EINVAL);
		memset(&lim, 0, sizeof(lim));
		lim.set = 1U << PIDMON_NLIM;
		EXPECT_ERR("unknown limit bit", pm_start_lim(0, 0, &lim), EINVAL);
		memset(&lim, 0, sizeof(lim));
		lim.lim[PIDMON_LIM_RSS].soft = 10;	/* given but not marked in "set": ignored, even if odd */
		lim.lim[PIDMON_LIM_RSS].hard = 5;
		lim_set(&lim, PIDMON_LIM_AS, 5, ~0ULL);
		rc = syscall(__NR_pidmon_start, 0, 0, &lim, 8);
		EXPECT_ERR("limits too small", rc < 0 ? errno : 0, EINVAL);
		rc = syscall(__NR_pidmon_start, 0, 0, &lim, 1 << 20);
		EXPECT_ERR("limits too big", rc < 0 ? errno : 0, E2BIG);
		rc = syscall(__NR_pidmon_start, 0, 0, NULL, sizeof(lim));
		EXPECT_ERR("size without limits", rc < 0 ? errno : 0, EINVAL);
		rc = syscall(__NR_pidmon_start, 0, 0, (void *)1, sizeof(lim));
		EXPECT_ERR("bad limits pointer", rc < 0 ? errno : 0, EFAULT);
		memset(fat, 0, sizeof(fat));
		memcpy(fat, &lim, sizeof(lim));
		fat[sizeof(lim) + 5] = 1;
		rc = syscall(__NR_pidmon_start, 0, 0, fat, sizeof(fat));
		EXPECT_ERR("unknown trailing field", rc < 0 ? errno : 0, E2BIG);
		EXPECT_ERR("nothing was armed by the failures", pm_read(0, 0, &u), ENOENT);

		/* a longer struct whose extra tail is zero is fine; the odd, unmarked RSS entry is ignored */
		fat[sizeof(lim) + 5] = 0;
		/* report-only: the 5-byte address-space limit would otherwise end this very process */
		rc = syscall(__NR_pidmon_start, 0, PIDMON_F_NO_ENFORCE, fat, sizeof(fat));
		EXPECT_ERR("zero tail", rc < 0 ? errno : 0, 0);
		EXPECT_ERR("read", pm_read(0, 0, &u), 0);
		CHECK(u.lim[PIDMON_LIM_AS].soft == 5 && u.lim[PIDMON_LIM_AS].hard == ~0ULL,
		      "AS limit %llu / %llx", (unsigned long long)u.lim[PIDMON_LIM_AS].soft,
		      (unsigned long long)u.lim[PIDMON_LIM_AS].hard);
		CHECK(u.lim[PIDMON_LIM_RSS].soft != 10, "an unmarked entry was used");
		EXPECT_ERR("stop", pm_read(0, PIDMON_F_STOP, &u), 0);
	}

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
