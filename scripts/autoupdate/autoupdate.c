#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int aconline(void);
static int lidclosed(void);
static int markupdated(void);
static int run(char *const argv[]);
static void sigterm(int sig);
static int updatedtoday(void);

#include "config.h"

static volatile sig_atomic_t running = 1;

static int
aconline(void)
{
	char c;
	int fd, r;

	if ((fd = open(AC_PATH, O_RDONLY | O_CLOEXEC)) < 0)
		return 0;
	r = read(fd, &c, 1) == 1 && c == '1';
	close(fd);

	return r;
}

static int
lidclosed(void)
{
	char buf[32];
	int fd;
	ssize_t n;

	if ((fd = open(LID_PATH, O_RDONLY | O_CLOEXEC)) < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';

	return strstr(buf, "closed") != NULL;
}

static int
markupdated(void)
{
	int fd;

	if ((fd = open(STAMP_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
	    0644)) < 0)
		return -1;
	close(fd);

	return 0;
}

static int
run(char *const argv[])
{
	int fd, status;
	pid_t pid;

	if ((pid = fork()) < 0)
		return -1;
	if (pid == 0) {
		if ((fd = open("/dev/null", O_RDONLY)) < 0)
			_exit(127);
		if (fd != 0) {
			dup2(fd, 0);
			close(fd);
		}
		execv(argv[0], argv);
		write(STDERR_FILENO, "autoupdate: execv failed\n",
		    sizeof("autoupdate: execv failed\n") - 1);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;

	return 0;
}

static void
sigterm(int sig)
{
	(void)sig;
	running = 0;
}

static int
updatedtoday(void)
{
	struct stat st;
	time_t d;

	if (stat(STAMP_PATH, &st) < 0)
		return 0;
	d = time(NULL) - st.st_mtime;

	return d >= 0 && d < COOLDOWN_SECS;
}

int
main(void)
{
	struct sigaction sa;
	struct timespec ts;
	time_t closed_since, retry_after;
	int needs_reboot;

	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sigterm;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	closed_since = -1;
	needs_reboot = 0;
	retry_after = 0;
	for (; running; sleep(SLEEP_SECS)) {
		if (!aconline() || !lidclosed()) {
			closed_since = -1;
			continue;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
			continue;
		if (closed_since < 0)
			closed_since = ts.tv_sec;
		if (ts.tv_sec - closed_since < IDLE_SECS)
			continue;
		if (needs_reboot) {
			needs_reboot = 0;
			run(cmd_reboot);
			continue;
		}
		if (ts.tv_sec < retry_after || updatedtoday())
			continue;
		if (run(cmd_update) == 0 && markupdated() == 0 && running) {
			if (lidclosed() && aconline())
				run(cmd_reboot);
			else
				needs_reboot = 1;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
			retry_after = ts.tv_sec + RETRY_SECS;
	}

	return 0;
}
