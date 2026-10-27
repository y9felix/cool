#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
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
        execlp("wl-paste", "wl-paste", "-t", "text", "-w", self_path, "--push", (char *)NULL);
        _exit(127);
    }
    wl_paste_pid = pid;
}

static void stop_wl_paste(void) {
    if (wl_paste_pid > 0) {
        kill(wl_paste_pid, SIGTERM);
        waitpid(wl_paste_pid, NULL, 0);
        wl_paste_pid = -1;
    }
}

static int is_any_device_connected(void) {
    FILE *fp = popen("adb devices", "r");
    if (!fp) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\tdevice")) {
            found = 1;
            break;
        }
    }
    pclose(fp);
    return found;
}

static int ensure_adb_device(const char *target_device) {
    if (is_any_device_connected()) return 1;
    if (target_device && *target_device) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "adb connect %s >/dev/null 2>&1", target_device);
        system(cmd);
        if (is_any_device_connected()) return 1;
    }
    FILE *fp = popen("nmap -T4 -p 30000-65535 192.168.0.123 2>/dev/null | awk -F'/' '/open/ {print $1; exit}'", "r");
    if (fp) {
        char port[32];
        if (fgets(port, sizeof(port), fp)) {
            int p = atoi(port);
            if (p > 0) {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "adb connect 192.168.0.123:%d >/dev/null 2>&1", p);
                system(cmd);
            }
        }
        pclose(fp);
    }
    return is_any_device_connected();
}

static int start_android_connection(const char *target_device, int port) {
    if (!ensure_adb_device(target_device)) return -1;
    system("adb push /usr/share/scrcpy/scrcpy-server /data/local/tmp/scrcpy-server.jar >/dev/null 2>&1");
    char fwd[256];
    snprintf(fwd, sizeof(fwd), "adb forward tcp:%d localabstract:scrcpy_00000001 >/dev/null 2>&1", port);
    system(fwd);
    system("adb shell \"CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server 5.0.1 scid=00000001 video=false audio=false control=true tunnel_forward=true send_device_meta=false send_dummy_byte=false cleanup=false\" >/dev/null 2>&1 </dev/null &");

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    for (int i = 0; i < 30; i++) {
        usleep(100000);
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return s;
        }
    }
    close(s);
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

    int running = 1;
    while (running) {
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

        int timeout_ms = (android_fd < 0) ? 3000 : -1;
        int pr = poll(fds, nfds, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pr == 0 && android_fd < 0) {
            android_fd = start_android_connection(target, android_port);
            continue;
        }

        if (fds[idx_sig].revents & POLLIN) {
            struct signalfd_siginfo sinfo;
            while (read(sig_fd, &sinfo, sizeof(sinfo)) > 0) {
                if (sinfo.ssi_signo == SIGCHLD) {
                    int st;
                    pid_t ch = waitpid(-1, &st, WNOHANG);
                    if (ch == wl_paste_pid) {
                        wl_paste_pid = -1;
                        start_wl_paste(self_path);
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
                continue;
            }
            if (type == 0) {
                uint32_t netlen;
                if (read_exact(android_fd, &netlen, 4) < 0) {
                    close(android_fd);
                    android_fd = -1;
                    continue;
                }
                uint32_t len = ntohl(netlen);
                if (len > 1048576) {
                    close(android_fd);
                    android_fd = -1;
                    continue;
                }
                char *text = malloc(len + 1);
                if (text) {
                    if (read_exact(android_fd, text, len) < 0) {
                        free(text);
                        close(android_fd);
                        android_fd = -1;
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
    system("adb forward --remove tcp:28888 >/dev/null 2>&1");
    return 0;
}
