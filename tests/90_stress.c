/* Races: several threads of one owner and several owners at once start, read, stop and kill
 * targets while those targets exit. Every call must return one of its documented results; run
 * under PROFILE=debug this is what shakes out lock-order, refcount and use-after-free bugs. */
#include "common.h"
#include <pthread.h>

#define NK 8
#define OPS 2500
#define WORKERS 3
#define ROUNDS 120

static volatile pid_t kids[NK];

static int in_set(int r, const int *ok)
{
	for (; *ok >= 0; ok++)
		if (r == *ok)
			return 1;
	return 0;
}

static const int start_ok[] = { 0, EBUSY, ESRCH, ENOSPC, -1 };
static const int read_ok[] = { 0, ENOENT, ESRCH, EPERM, -1 };

static void *hammer(void *arg)
{
	unsigned int seed = (unsigned long)arg;
	struct pidmon_usage u;
	int i, r;

	for (i = 0; i < OPS; i++) {
		pid_t p = kids[rand_r(&seed) % NK];

		switch (rand_r(&seed) % 5) {
		case 0:
		case 1:
			r = pm_start(p, 0);
			CHECK(in_set(r, start_ok), "start(%d) -> %d (%s)", p, r, strerror(r));
			break;
		case 2:
			r = pm_read(p, PIDMON_F_NO_ENFORCE, &u);
			CHECK(in_set(r, read_ok), "read(%d) -> %d (%s)", p, r, strerror(r));
			break;
		case 3:
			r = pm_read(p, PIDMON_F_STOP | PIDMON_F_NO_ENFORCE, &u);
			CHECK(in_set(r, read_ok), "read STOP(%d) -> %d (%s)", p, r, strerror(r));
			break;
		default:
			r = pm_read(0, PIDMON_F_NO_ENFORCE, &u);	/* self, usually no record */
			CHECK(in_set(r, read_ok), "read self -> %d (%s)", r, strerror(r));
			break;
		}
	}
	return NULL;
}

static void fresh_kid(int k)
{
	int fd;

	kids[k] = spawn(sleeper, NULL, &fd);
	get_byte(fd, 2000);
	close(fd);
}

/* its own owner process: a tree of short-lived children, monitored and killed at random */
static void worker(int in, int out)
{
	unsigned int seed = getpid();
	struct pidmon_usage u;
	int i, r;

	(void)in;
	for (i = 0; i < ROUNDS; i++) {
		pid_t c = fork();

		if (c == 0) {
			if (rand_r(&seed) % 2)
				burn_ms(1);
			if (rand_r(&seed) % 2 && fork() == 0)
				_exit(0);		/* a grandchild */
			_exit(0);
		}
		r = pm_start(c, 0);
		CHECK(in_set(r, start_ok), "worker start -> %d", r);
		if (rand_r(&seed) % 3 == 0)
			kill(c, SIGKILL);
		if (rand_r(&seed) % 2) {
			r = pm_read(c, PIDMON_F_NO_ENFORCE, &u);
			CHECK(in_set(r, read_ok), "worker read -> %d", r);
		}
		if (rand_r(&seed) % 2)
			waitpid(c, NULL, 0);
		if (rand_r(&seed) % 2) {
			r = pm_read(c, PIDMON_F_STOP | PIDMON_F_NO_ENFORCE, &u);
			CHECK(in_set(r, read_ok), "worker read STOP -> %d", r);
		}
		if (i % 8 == 0)		/* the others pile up and are freed when we exit */
			while (waitpid(-1, NULL, WNOHANG) > 0)
				;
	}
	put_byte(out, failures ? 'F' : 'k');
}

int main(void)
{
	pthread_t th[4];
	pid_t w[WORKERS];
	int from[WORKERS], i, k, st;

	test_begin("90_stress");

	for (k = 0; k < NK; k++)
		fresh_kid(k);
	for (i = 0; i < WORKERS; i++)
		w[i] = spawn(worker, NULL, &from[i]);
	for (i = 0; i < 4; i++)
		pthread_create(&th[i], NULL, hammer, (void *)(long)(i + 1));

	/* meanwhile keep killing and replacing the targets the threads are hammering */
	for (i = 0; i < 60; i++) {
		k = i % NK;
		kill(kids[k], SIGKILL);
		waitpid(kids[k], NULL, 0);
		fresh_kid(k);
		msleep(5);
	}

	for (i = 0; i < 4; i++)
		pthread_join(th[i], NULL);
	for (i = 0; i < WORKERS; i++) {
		CHECK(get_byte(from[i], 60000) == 'k', "worker %d reported failures", i);
		waitpid(w[i], &st, 0);
	}
	for (k = 0; k < NK; k++)
		kill_reap(kids[k]);

	return test_end();
}
