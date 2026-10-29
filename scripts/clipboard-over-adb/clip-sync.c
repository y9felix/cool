#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define CLIP_MAX 1048576
#define ANDROID_PORT 28888
#define DEFAULT_HOST "192.168.0.123"
#define DEFAULT_PORT 5555
#define SCRCPY_VER "5.0.1"
#define SCRCPY_SCID "00000001"
#define SCRCPY_LOCAL "/usr/share/scrcpy/scrcpy-server"
#define SCRCPY_REMOTE "/data/local/tmp/scrcpy-server.jar"

static void die(const char *fmt, ...);
static void sighandler(int sig);
static void reap_children(void);
static uint64_t monoms(void);
static uint64_t fnv1a(const char *s, size_t len);
static int write_all(int fd, const void *buf, size_t len);
static int read_exact(int fd, void *buf, size_t len);
static pid_t spawn(char *const argv[]);
static int run_cmd(char *const argv[]);
static void kill_wait(pid_t *pid);
static void start_wl_paste(const char *self);
static void spawn_scrcpy(void);
static int probe_device(const char *ip, int port);
static int ensure_adb(const char *target);
static void disconnect_android(void);
static int connect_android(const char *target);
static int send_to_android(const char *text, size_t len);
static void wl_copy(const char *data, size_t len);
static int run_push(const char *sock_path);
static void handle_push(int srv_fd);
static void handle_android(void);

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t sigchld_flag;
static uint64_t last_hash;
static char recv_buf[CLIP_MAX + 1];
static pid_t wl_paste_pid = -1;
static uint64_t wl_paste_retry;
static pid_t scrcpy_pid = -1;
static int android_fd = -1;
static uint64_t android_retry;
static int android_backoff = 1000;

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	exit(1);
}

static void
sighandler(int sig)
{
	if (sig == SIGCHLD)
		sigchld_flag = 1;
	else
		running = 0;
}

static void
reap_children(void)
{
	int st;
	pid_t ch;

	while ((ch = waitpid(-1, &st, WNOHANG)) > 0) {
		if (ch == wl_paste_pid) {
			wl_paste_pid = -1;
			wl_paste_retry = monoms() + 1000;
		} else if (ch == scrcpy_pid) {
			scrcpy_pid = -1;
		}
	}
}

static uint64_t
monoms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t
fnv1a(const char *s, size_t len)
{
	uint64_t h = 14695981039346656037ULL;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= (uint8_t)s[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int
write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t n;

	while (len > 0) {
		if ((n = write(fd, p, len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static int
read_exact(int fd, void *buf, size_t len)
{
	char *p = buf;
	ssize_t n;

	while (len > 0) {
		if ((n = read(fd, p, len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static pid_t
spawn(char *const argv[])
{
	int devnull;
	pid_t pid;

	if ((pid = fork()) == 0) {
		if ((devnull = open("/dev/null", O_RDWR)) >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 1);
			dup2(devnull, 2);
			close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static int
run_cmd(char *const argv[])
{
	int status;
	pid_t pid = spawn(argv);

	if (pid < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void
kill_wait(pid_t *pid)
{
	if (*pid > 0) {
		kill(*pid, SIGTERM);
		while (waitpid(*pid, NULL, 0) < 0) {
			if (errno != EINTR)
				break;
		}
		*pid = -1;
	}
}

static void
start_wl_paste(const char *self)
{
	char *args[] = {"wl-paste", "-t", "text", "-w", (char *)self, "--push", NULL};

	if (wl_paste_pid <= 0)
		wl_paste_pid = spawn(args);
}

static void
spawn_scrcpy(void)
{
	char *args[] = {
		"adb", "shell",
		"CLASSPATH=" SCRCPY_REMOTE " app_process / "
		"com.genymobile.scrcpy.Server " SCRCPY_VER " scid=" SCRCPY_SCID
		" video=false audio=false control=true tunnel_forward=true"
		" send_device_meta=false send_dummy_byte=false cleanup=false",
		NULL
	};

	kill_wait(&scrcpy_pid);
	scrcpy_pid = spawn(args);
}

static int
probe_device(const char *ip, int port)
{
	struct sockaddr_in addr;
	struct pollfd pfd;
	socklen_t len = sizeof(int);
	int fd, err = 0;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return 0;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
		close(fd);
		return 0;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (errno != EINPROGRESS) {
			close(fd);
			return 0;
		}
		pfd.fd = fd;
		pfd.events = POLLOUT;
		if (poll(&pfd, 1, 300) <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
			close(fd);
			return 0;
		}
	}
	close(fd);
	return 1;
}

static int
ensure_adb(const char *target)
{
	char ip[64], spec[128], *colon;
	char *args[] = {"adb", "connect", spec, NULL};
	int port = DEFAULT_PORT;

	snprintf(ip, sizeof(ip), "%s", (target && *target) ? target : DEFAULT_HOST);
	if ((colon = strchr(ip, ':'))) {
		*colon = '\0';
		port = atoi(colon + 1);
		if (port <= 0 || port > 65535)
			port = DEFAULT_PORT;
	}
	if (!probe_device(ip, port))
		return 0;
	snprintf(spec, sizeof(spec), "%s:%d", ip, port);
	return run_cmd(args) == 0;
}

static void
disconnect_android(void)
{
	char fwd[32];
	char *rm_args[] = {"adb", "forward", "--remove", fwd, NULL};

	if (android_fd >= 0) {
		close(android_fd);
		android_fd = -1;
	}
	kill_wait(&scrcpy_pid);
	snprintf(fwd, sizeof(fwd), "tcp:%d", ANDROID_PORT);
	run_cmd(rm_args);
	android_retry = monoms() + android_backoff;
	android_backoff = MIN(android_backoff * 2, 10000);
}

static int
connect_android(const char *target)
{
	struct sockaddr_in addr;
	struct pollfd pfd;
	char fwd[32], scid[32], b;
	char *push_args[] = {"adb", "push", SCRCPY_LOCAL, SCRCPY_REMOTE, NULL};
	char *fwd_args[] = {"adb", "forward", fwd, scid, NULL};
	int i, s;

	if (!ensure_adb(target)) {
		android_retry = monoms() + android_backoff;
		android_backoff = MIN(android_backoff * 2, 10000);
		return -1;
	}

	snprintf(fwd, sizeof(fwd), "tcp:%d", ANDROID_PORT);
	snprintf(scid, sizeof(scid), "localabstract:scrcpy_%s", SCRCPY_SCID);

	run_cmd(push_args);
	run_cmd(fwd_args);
	spawn_scrcpy();

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ANDROID_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	for (i = 0; i < 30; i++) {
		if (!running) {
			disconnect_android();
			return -1;
		}
		poll(NULL, 0, 100);
		if (!running) {
			disconnect_android();
			return -1;
		}
		if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
			continue;
		if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			pfd.fd = s;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 100) > 0 && recv(s, &b, 1, MSG_PEEK) <= 0) {
				close(s);
				continue;
			}
			android_fd = s;
			android_backoff = 1000;
			android_retry = 0;
			return 0;
		}
		close(s);
	}

	disconnect_android();
	return -1;
}

static int
send_to_android(const char *text, size_t len)
{
	struct iovec iov[2];
	uint8_t hdr[14];
	uint32_t nlen;
	ssize_t n;
	size_t total;

	if (android_fd < 0 || len == 0)
		return 0;

	hdr[0] = 9;
	memset(hdr + 1, 0, 9);
	nlen = htonl((uint32_t)len);
	memcpy(hdr + 10, &nlen, 4);

	iov[0].iov_base = hdr;
	iov[0].iov_len = sizeof(hdr);
	iov[1].iov_base = (void *)text;
	iov[1].iov_len = len;
	total = sizeof(hdr) + len;

	while (total > 0) {
		if ((n = writev(android_fd, iov, 2)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		total -= n;
		if ((size_t)n >= iov[0].iov_len) {
			n -= iov[0].iov_len;
			iov[0].iov_len = 0;
			iov[1].iov_base = (char *)iov[1].iov_base + n;
			iov[1].iov_len -= n;
		} else {
			iov[0].iov_base = (char *)iov[0].iov_base + n;
			iov[0].iov_len -= n;
		}
	}
	return 0;
}

static void
wl_copy(const char *data, size_t len)
{
	int p[2];
	pid_t pid;

	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		close(p[1]);
		dup2(p[0], 0);
		close(p[0]);
		execlp("wl-copy", "wl-copy", (char *)NULL);
		_exit(127);
	}
	close(p[0]);
	if (pid > 0) {
		write_all(p[1], data, len);
		close(p[1]);
		while (waitpid(pid, NULL, 0) < 0) {
			if (errno != EINTR)
				break;
		}
	} else {
		close(p[1]);
	}
}

static int
run_push(const char *sock_path)
{
	struct sockaddr_un addr;
	char buf[8192];
	ssize_t n;
	int fd;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return 1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return 1;
	}
	for (;;) {
		if ((n = read(0, buf, sizeof(buf))) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		if (write_all(fd, buf, n) < 0)
			break;
	}
	close(fd);
	return 0;
}

static void
handle_push(int srv_fd)
{
	uint64_t h;
	size_t len = 0;
	ssize_t n;
	int cfd;

	if ((cfd = accept(srv_fd, NULL, NULL)) < 0)
		return;
	while (len < CLIP_MAX) {
		if ((n = read(cfd, recv_buf + len, CLIP_MAX - len)) <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		len += n;
	}
	close(cfd);

	if (len > 0) {
		h = fnv1a(recv_buf, len);
		if (h != last_hash) {
			last_hash = h;
			if (send_to_android(recv_buf, len) < 0)
				disconnect_android();
		}
	}
}

static void
handle_android(void)
{
	uint64_t h;
	uint32_t netlen, rlen;
	uint16_t dlen;
	uint8_t type;
	char dummy[512];
	size_t chunk;

	if (read_exact(android_fd, &type, 1) < 0)
		goto fail;

	if (type == 0) {
		if (read_exact(android_fd, &netlen, 4) < 0)
			goto fail;
		rlen = ntohl(netlen);
		if (rlen > CLIP_MAX || read_exact(android_fd, recv_buf, rlen) < 0)
			goto fail;
		recv_buf[rlen] = '\0';
		h = fnv1a(recv_buf, rlen);
		if (h != last_hash) {
			last_hash = h;
			wl_copy(recv_buf, rlen);
		}
	} else if (type == 1) {
		if (read_exact(android_fd, dummy, 8) < 0)
			goto fail;
	} else if (type == 2) {
		if (read_exact(android_fd, dummy, 2) < 0 ||
		    read_exact(android_fd, &dlen, 2) < 0)
			goto fail;
		dlen = ntohs(dlen);
		while (dlen > 0) {
			chunk = MIN(dlen, sizeof(dummy));
			if (read_exact(android_fd, dummy, chunk) < 0)
				goto fail;
			dlen -= chunk;
		}
	} else {
		goto fail;
	}
	return;

fail:
	disconnect_android();
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	struct sockaddr_un saddr;
	struct pollfd fds[2];
	const char *runtime_dir, *target;
	char sock_path[sizeof(saddr.sun_path)];
	uint64_t now;
	int srv_fd;
	int nfds, idx_srv, idx_android, timeout, t;

	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir && *runtime_dir)
		snprintf(sock_path, sizeof(sock_path), "%s/clip-sync.sock", runtime_dir);
	else
		snprintf(sock_path, sizeof(sock_path), "/tmp/clip-sync-%d.sock", (int)getuid());

	if (argc > 1 && strcmp(argv[1], "--push") == 0)
		return run_push(sock_path);

	target = (argc > 1 && argv[1][0] != '-') ? argv[1] : getenv("CLIP_SYNC_DEVICE");

	signal(SIGPIPE, SIG_IGN);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	unlink(sock_path);
	if ((srv_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		die("socket:");
	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", sock_path);
	if (bind(srv_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
		die("bind:");
	if (listen(srv_fd, 16) < 0)
		die("listen:");

	start_wl_paste(argv[0]);
	connect_android(target);

	for (;;) {
		if (sigchld_flag) {
			sigchld_flag = 0;
			reap_children();
		}
		if (!running)
			break;

		now = monoms();

		if (wl_paste_pid < 0 && now >= wl_paste_retry) {
			start_wl_paste(argv[0]);
			now = monoms();
		}
		if (android_fd < 0 && now >= android_retry) {
			connect_android(target);
			now = monoms();
		}

		nfds = 0;
		fds[nfds].fd = srv_fd;
		fds[nfds].events = POLLIN;
		idx_srv = nfds++;

		idx_android = -1;
		if (android_fd >= 0) {
			fds[nfds].fd = android_fd;
			fds[nfds].events = POLLIN;
			idx_android = nfds++;
		}

		timeout = -1;
		if (android_fd < 0)
			timeout = (now >= android_retry) ? 0 : (int)(android_retry - now);
		if (wl_paste_pid < 0) {
			t = (now >= wl_paste_retry) ? 0 : (int)(wl_paste_retry - now);
			timeout = (timeout < 0) ? t : MIN(timeout, t);
		}

		if (poll(fds, nfds, timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (fds[idx_srv].revents & POLLIN)
			handle_push(srv_fd);
		if (idx_android >= 0 && (fds[idx_android].revents & (POLLIN | POLLHUP | POLLERR)))
			handle_android();
	}

	kill_wait(&wl_paste_pid);
	disconnect_android();
	close(srv_fd);
	unlink(sock_path);
	return 0;
}
