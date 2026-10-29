#define IDLE_SECS      14400
#define RETRY_SECS     900
#define SLEEP_SECS     30
#define COOLDOWN_SECS  64800

#define AC_PATH        "/sys/class/power_supply/AC/online"
#define LID_PATH       "/proc/acpi/button/lid/LID/state"
#define STAMP_PATH     "/var/lib/autoupdate.stamp"

static char *const cmd_reboot[] = { "/usr/bin/systemctl", "reboot", NULL };
static char *const cmd_update[] = {
	"/usr/bin/pacman", "-Syu", "--noconfirm", "--noprogressbar", NULL
};
