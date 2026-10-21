#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IDLE_SECS 14400
#define STAMP_FILE "/var/lib/autoupdate.stamp"

static int is_updated_today(void) {
    char today[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(today, sizeof(today), "%Y-%m-%d", &tm);

    FILE *f = fopen(STAMP_FILE, "r");
    if (!f) return 0;

    char buf[16] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        fclose(f);
        return strcmp(today, buf) == 0;
    }
    fclose(f);
    return 0;
}

static void mark_updated_today(void) {
    char today[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(today, sizeof(today), "%Y-%m-%d", &tm);

    FILE *f = fopen(STAMP_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", today);
        fclose(f);
    }
}

static int is_lid_closed(void) {
    FILE *f = fopen("/proc/acpi/button/lid/LID/state", "r");
    if (!f) return 0;
    char buf[64];
    int closed = 0;
    if (fgets(buf, sizeof(buf), f)) {
        closed = (strstr(buf, "closed") != NULL);
    }
    fclose(f);
    return closed;
}

static int is_ac_online(void) {
    FILE *f = fopen("/sys/class/power_supply/AC/online", "r");
    if (!f) return 0;
    int online = 0;
    if (fscanf(f, "%d", &online) != 1) {
        online = 0;
    }
    fclose(f);
    return online == 1;
}

int main(void) {
    struct timespec ts;
    time_t closed_since = 0;
    time_t retry_after = 0;

    while (1) {
        if (!is_lid_closed()) {
            closed_since = 0;
        } else {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if (closed_since == 0) {
                closed_since = ts.tv_sec;
            } else if ((ts.tv_sec - closed_since) >= IDLE_SECS) {
                if (!is_updated_today() && is_ac_online() && ts.tv_sec >= retry_after) {
                    if (system("/usr/bin/pacman -Syu --noconfirm") == 0) {
                        mark_updated_today();
                        system("/usr/bin/systemctl reboot");
                    }
                    retry_after = ts.tv_sec + 900;
                }
            }
        }
        sleep(30);
    }

    return 0;
}
