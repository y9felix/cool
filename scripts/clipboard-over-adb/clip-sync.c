#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>

static char *last_clip = NULL;
static size_t last_clip_len = 0;
static pid_t wl_paste_pid = -1;
static uint64_t wl_paste_start_time = 0;
static uint64_t wl_paste_retry_at_ms = 0;
static int wl_paste_backoff_ms = 1000;

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int is_duplicate(const char *text, size_t len) {
    if (!last_clip || last_clip_len != len) return 0;
    return memcmp(last_clip, text, len) == 0;
}

static void update_last_clip(const char *text, size_t len) {
    char *dup = malloc(len + 1);
    if (!dup) return;
    memcpy(dup, text, len);
    dup[len] = '\0';
    free(last_clip);
    last_clip = dup;
    last_clip_len = len;
}

static void wl_copy(const char *data, size_t len) {
    int p[2];
    if (pipe(p) < 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigprocmask(SIG_SETMASK, &unblock, NULL);
        close(p[1]);
        dup2(p[0], STDIN_FILENO);
        close(p[0]);
        execlp("wl-copy", "wl-copy", (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid > 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(p[1], data + off, len - off);
            if (w <= 0) break;
            off += w;
        }
        close(p[1]);
        waitpid(pid, NULL, 0);
    } else {
        close(p[1]);
    }
}

static int send_to_android(int sock, const char *text, size_t len) {
    if (sock < 0 || len == 0) return 0;
    size_t total = 14 + len;
    uint8_t *pkt = malloc(total);
    if (!pkt) return -1;
    pkt[0] = 9;
    memset(&pkt[1], 0, 8);
    pkt[9] = 0;
    uint32_t nlen = htonl((uint32_t)len);
    memcpy(&pkt[10], &nlen, 4);
    memcpy(&pkt[14], text, len);
    size_t written = 0;
    while (written < total) {
        ssize_t res = write(sock, pkt + written, total - written);
        if (res <= 0) {
            free(pkt);
            return -1;
        }
        written += res;
    }
    free(pkt);
    return 0;
}

static int read_exact(int fd, void *buf, size_t count) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = read(fd, (char *)buf + done, count - done);
        if (n <= 0) return -1;
        done += n;
    }
    return 0;
}

static int run_push(const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 1;
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, buf + off, n - off);
            if (w <= 0) break;
            off += w;
        }
    }
    close(fd);
    return 0;
}

static void start_wl_paste(const char *self_path) {
    if (wl_paste_pid > 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigprocmask(SIG_SETMASK, &unblock, NULL);
        execlp("wl-paste", "wl-paste", "-t", "text", "-w", self_path, "--push", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        wl_paste_pid = pid;
        wl_paste_start_time = get_monotonic_ms();
    }
}

static void stop_wl_paste(void) {
    if (wl_paste_pid > 0) {
        kill(wl_paste_pid, SIGTERM);
        waitpid(wl_paste_pid, NULL, 0);
        wl_paste_pid = -1;
    }
}

static int run_cmd(const char *file, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigprocmask(SIG_SETMASK, &unblock, NULL);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(file, argv);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int probe_device(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd);
        return 0;
    }

    int res = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return 0;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int ret = poll(&pfd, 1, 300);
        if (ret <= 0) {
            close(fd);
            return 0;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(fd);
            return 0;
        }
    }
    close(fd);
    return 1;
}

static int ensure_adb_device(const char *target_device) {
    char ip[64] = "192.168.0.123";
    int port = 5555;
    if (target_device && *target_device) {
        char buf[64];
        strncpy(buf, target_device, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *colon = strchr(buf, ':');
        if (colon) {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0 && p <= 65535) port = p;
        }
        strncpy(ip, buf, sizeof(ip) - 1);
        ip[sizeof(ip) - 1] = '\0';
    }
    if (!probe_device(ip, port)) return 0;
    char target[128];
    snprintf(target, sizeof(target), "%s:%d", ip, port);
    char *args[] = {"adb", "connect", target, NULL};
    return (run_cmd("adb", args) == 0);
}

static void spawn_scrcpy_server(void) {
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigprocmask(SIG_SETMASK, &unblock, NULL);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("adb", "adb", "shell",
               "CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / "
               "com.genymobile.scrcpy.Server 5.0.1 scid=00000001 video=false audio=false "
               "control=true tunnel_forward=true send_device_meta=false send_dummy_byte=false cleanup=false",
               (char *)NULL);
        _exit(127);
    }
}

static int start_android_connection(const char *target_device, int port) {
    if (!ensure_adb_device(target_device)) return -1;

    char *push_args[] = {"adb", "push", "/usr/share/scrcpy/scrcpy-server", "/data/local/tmp/scrcpy-server.jar", NULL};
    run_cmd("adb", push_args);

    char fwd_spec[32];
    snprintf(fwd_spec, sizeof(fwd_spec), "tcp:%d", port);
    char *fwd_args[] = {"adb", "forward", fwd_spec, "localabstract:scrcpy_00000001", NULL};
    run_cmd("adb", fwd_args);

    spawn_scrcpy_server();

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    for (int i = 0; i < 30; i++) {
        usleep(100000);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) continue;
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return s;
        }
        close(s);
    }

    char *rm_args[] = {"adb", "forward", "--remove", fwd_spec, NULL};
    run_cmd("adb", rm_args);
    return -1;
}

int main(int argc, char **argv) {
    char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && *runtime_dir) {
        snprintf(sock_path, sizeof(sock_path), "%s/clip-sync.sock", runtime_dir);
    } else {
        snprintf(sock_path, sizeof(sock_path), "/tmp/clip-sync-%d.sock", (int)getuid());
    }

    if (argc > 1 && strcmp(argv[1], "--push") == 0) {
        return run_push(sock_path);
    }

    char self_path[PATH_MAX];
    ssize_t rl = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (rl > 0) self_path[rl] = '\0';
    else snprintf(self_path, sizeof(self_path), "/usr/local/bin/clip-sync");

    const char *target = NULL;
    if (argc > 1 && argv[1][0] != '-') {
        target = argv[1];
    } else {
        target = getenv("CLIP_SYNC_DEVICE");
    }

    unlink(sock_path);
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) return 1;
    struct sockaddr_un saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", sock_path);
    if (bind(srv_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        close(srv_fd);
        return 1;
    }
    if (listen(srv_fd, 16) < 0) {
        close(srv_fd);
        unlink(sock_path);
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK);

    start_wl_paste(self_path);

    int android_port = 28888;
    int android_fd = start_android_connection(target, android_port);
    int android_backoff_ms = 1000;
    uint64_t android_retry_at_ms = 0;
    if (android_fd < 0) {
        android_retry_at_ms = get_monotonic_ms() + android_backoff_ms;
        android_backoff_ms = (android_backoff_ms * 2 > 10000) ? 10000 : android_backoff_ms * 2;
    }

    int running = 1;
    while (running) {
        uint64_t now = get_monotonic_ms();

        if (wl_paste_pid < 0 && now >= wl_paste_retry_at_ms) {
            start_wl_paste(self_path);
            now = get_monotonic_ms();
        }

        if (android_fd < 0 && now >= android_retry_at_ms) {
            android_fd = start_android_connection(target, android_port);
            now = get_monotonic_ms();
            if (android_fd >= 0) {
                android_backoff_ms = 1000;
                android_retry_at_ms = 0;
            } else {
                android_retry_at_ms = now + android_backoff_ms;
                android_backoff_ms = (android_backoff_ms * 2 > 10000) ? 10000 : android_backoff_ms * 2;
            }
        }

        struct pollfd fds[3];
        int nfds = 0;

        fds[nfds].fd = sig_fd;
        fds[nfds].events = POLLIN;
        int idx_sig = nfds++;

        fds[nfds].fd = srv_fd;
        fds[nfds].events = POLLIN;
        int idx_srv = nfds++;

        int idx_android = -1;
        if (android_fd >= 0) {
            fds[nfds].fd = android_fd;
            fds[nfds].events = POLLIN;
            idx_android = nfds++;
        }

        int timeout_ms = -1;
        if (android_fd < 0) {
            int t_android = (now >= android_retry_at_ms) ? 0 : (int)(android_retry_at_ms - now);
            timeout_ms = t_android;
        }
        if (wl_paste_pid < 0) {
            int t_wl = (now >= wl_paste_retry_at_ms) ? 0 : (int)(wl_paste_retry_at_ms - now);
            if (timeout_ms < 0 || t_wl < timeout_ms) {
                timeout_ms = t_wl;
            }
        }

        int pr = poll(fds, nfds, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[idx_sig].revents & POLLIN) {
            struct signalfd_siginfo sinfo;
            while (read(sig_fd, &sinfo, sizeof(sinfo)) > 0) {
                if (sinfo.ssi_signo == SIGCHLD) {
                    int st;
                    pid_t ch;
                    while ((ch = waitpid(-1, &st, WNOHANG)) > 0) {
                        if (ch == wl_paste_pid) {
                            wl_paste_pid = -1;
                            uint64_t cur = get_monotonic_ms();
                            if (cur - wl_paste_start_time < 2000) {
                                wl_paste_retry_at_ms = cur + wl_paste_backoff_ms;
                                wl_paste_backoff_ms = (wl_paste_backoff_ms * 2 > 10000) ? 10000 : wl_paste_backoff_ms * 2;
                            } else {
                                wl_paste_backoff_ms = 1000;
                                wl_paste_retry_at_ms = 0;
                                start_wl_paste(self_path);
                            }
                        }
                    }
                } else if (sinfo.ssi_signo == SIGTERM || sinfo.ssi_signo == SIGINT || sinfo.ssi_signo == SIGHUP) {
                    running = 0;
                    break;
                }
            }
            if (!running) break;
        }

        if (fds[idx_srv].revents & POLLIN) {
            int cfd = accept(srv_fd, NULL, NULL);
            if (cfd >= 0) {
                size_t cap = 4096;
                size_t len = 0;
                char *buf = malloc(cap);
                if (buf) {
                    ssize_t n;
                    while ((n = read(cfd, buf + len, cap - len)) > 0) {
                        len += n;
                        if (len == cap) {
                            if (cap >= 1048576) break;
                            cap *= 2;
                            char *nb = realloc(buf, cap);
                            if (!nb) break;
                            buf = nb;
                        }
                    }
                    if (len > 0 && !is_duplicate(buf, len)) {
                        update_last_clip(buf, len);
                        if (android_fd >= 0) {
                            if (send_to_android(android_fd, buf, len) < 0) {
                                close(android_fd);
                                android_fd = -1;
                                android_backoff_ms = 1000;
                                android_retry_at_ms = get_monotonic_ms() + android_backoff_ms;
                            }
                        }
                    }
                    free(buf);
                }
                close(cfd);
            }
        }

        if (idx_android >= 0 && (fds[idx_android].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t type;
            if (read_exact(android_fd, &type, 1) < 0) {
                close(android_fd);
                android_fd = -1;
                android_backoff_ms = 1000;
                android_retry_at_ms = get_monotonic_ms() + android_backoff_ms;
                continue;
            }
            if (type == 0) {
                uint32_t netlen;
                if (read_exact(android_fd, &netlen, 4) < 0) {
                    close(android_fd);
                    android_fd = -1;
                    android_backoff_ms = 1000;
                    android_retry_at_ms = get_monotonic_ms() + android_backoff_ms;
                    continue;
                }
                uint32_t len = ntohl(netlen);
                if (len > 1048576) {
                    close(android_fd);
                    android_fd = -1;
                    android_backoff_ms = 1000;
                    android_retry_at_ms = get_monotonic_ms() + android_backoff_ms;
                    continue;
                }
                char *text = malloc(len + 1);
                if (text) {
                    if (read_exact(android_fd, text, len) < 0) {
                        free(text);
                        close(android_fd);
                        android_fd = -1;
                        android_backoff_ms = 1000;
                        android_retry_at_ms = get_monotonic_ms() + android_backoff_ms;
                        continue;
                    }
                    text[len] = '\0';
                    if (!is_duplicate(text, len)) {
                        update_last_clip(text, len);
                        wl_copy(text, len);
                    }
                    free(text);
                }
            } else if (type == 1) {
                uint64_t seq;
                read_exact(android_fd, &seq, 8);
            } else if (type == 2) {
                uint16_t id, dlen;
                read_exact(android_fd, &id, 2);
                read_exact(android_fd, &dlen, 2);
                dlen = ntohs(dlen);
                char dummy[1024];
                while (dlen > 0) {
                    size_t chunk = dlen > sizeof(dummy) ? sizeof(dummy) : dlen;
                    if (read_exact(android_fd, dummy, chunk) < 0) break;
                    dlen -= chunk;
                }
            }
        }
    }

    stop_wl_paste();
    if (android_fd >= 0) close(android_fd);
    close(srv_fd);
    unlink(sock_path);
    close(sig_fd);
    free(last_clip);
    char fwd_spec[32];
    snprintf(fwd_spec, sizeof(fwd_spec), "tcp:%d", android_port);
    char *rm_args[] = {"adb", "forward", "--remove", fwd_spec, NULL};
    run_cmd("adb", rm_args);
    return 0;
}
