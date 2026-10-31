#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define N noop
#define BIND(iface) wl_registry_bind(r, n, &(iface), 1)
#define PI 3.14159265358979323846
#define KAPPA 0.552285
#define BATTERY_WIDTH 1.25

enum {
	CORNER_TOP_LEFT,
	CORNER_TOP_RIGHT,
	CORNER_BOTTOM_LEFT,
	CORNER_BOTTOM_RIGHT
};

static void noop();
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *l);
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t sx, wl_fixed_t sy);
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf);
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy);
static void ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st);
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t val);
static void ls_cfg(void *d, struct zwlr_layer_surface_v1 *l, uint32_t s, uint32_t w, uint32_t h);
static void seat_caps(void *d, struct wl_seat *s, uint32_t c);
static void reg_glob(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v);
static int read_file_str(const char *path, char *buf, size_t sz);
static void setup_mixer(void);
static int setup_netlink(void);
static void arm_timer(int enable);
static int get_brightness(void);
static void set_brightness_delta(int delta);
static int update_brightness(void);
static int get_volume(int *muted);
static void set_volume_delta(int delta);
static void toggle_mute(void);
static int update_volume(void);
static int get_battery(int *charging);
static int update_battery(void);
static void get_time(char *buf, size_t sz);
static int update_clock(void);
static void handle_timer(void);
static void handle_netlink(void);
static void handle_alsa(void);
static int level_brightness(int pct);
static int level_volume(int pct);
static int level_battery(int pct);
static void rounded_rect(cairo_t *cr, double x0, double y0, double x1, double y1, double r);
static void draw_icon_brightness(cairo_t *cr, double x, double y, double sz, int lvl);
static void draw_icon_volume(cairo_t *cr, double x, double y, double sz, int lvl, int muted);
static void draw_icon_battery(cairo_t *cr, double x, double y, double sz, int lvl, int charging);
static void draw_icon_clock(cairo_t *cr, double x, double y, double sz);
static void update_input_region(void);
static void draw(void);
static void clear_surface(void);
static int create_shm_buffer(void);
static void setup_wayland(void);
static void cleanup(void);
static void sig_handler(int sig);
static void usage(void);
static void die(const char *fmt, ...);

#include "config.h"

const struct wl_interface xdg_popup_interface;

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *shell;
static struct wl_surface *surf;
static struct zwlr_layer_surface_v1 *ls;
static struct wl_buffer *buf;
static struct wl_pointer *ptr;
static cairo_surface_t *back_surf;
static cairo_t *back_cr;
static snd_mixer_t *mixer;
static snd_mixer_elem_t *mixer_elem;
static void *pixel_data;
static size_t buffer_size;
static int timer_fd = -1;
static int netlink_fd = -1;
static int cur_brightness = -1;
static int cur_volume = -1;
static int cur_muted = -1;
static int cur_battery = -1;
static int cur_charging = -1;
static char cur_time[32];
static int selected_corner = corner;
static int opt_autohide = autohide;
static int visible;
static int configured;
static int pointer_x;
static int mod_x_bounds[2];
static volatile sig_atomic_t running = 1;

static const struct wl_pointer_listener ptr_l = { ptr_enter, ptr_leave, ptr_motion, ptr_btn, ptr_axis, N, N, N, N, N, N, N };
static const struct zwlr_layer_surface_v1_listener ls_l = { ls_cfg, ls_closed };
static const struct wl_seat_listener seat_l = { seat_caps, N };
static const struct wl_registry_listener reg_l = { reg_glob, N };

static void
noop()
{
}

static void
ls_closed(void *d, struct zwlr_layer_surface_v1 *l)
{
	running = 0;
}

static void
ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t sx, wl_fixed_t sy)
{
	pointer_x = wl_fixed_to_int(sx);
	if (!visible) {
		visible = 1;
		arm_timer(1);
		update_input_region();
		update_brightness();
		update_volume();
		update_battery();
		update_clock();
		draw();
	}
}

static void
ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{
	if (opt_autohide && visible) {
		visible = 0;
		arm_timer(0);
		update_input_region();
		clear_surface();
	}
}

static void
ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy)
{
	pointer_x = wl_fixed_to_int(sx);
}

static void
ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{
	if (st && b == 0x110) {
		if (pointer_x >= mod_x_bounds[0] && pointer_x < mod_x_bounds[1]) {
			toggle_mute();
			if (update_volume() && visible)
				draw();
		}
	}
}

static void
ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t val)
{
	if (val == 0)
		return;
	if (pointer_x < mod_x_bounds[0]) {
		set_brightness_delta(val < 0 ? 1 : -1);
		if (update_brightness() && visible)
			draw();
	} else if (pointer_x < mod_x_bounds[1]) {
		set_volume_delta(val < 0 ? 1 : -1);
		if (update_volume() && visible)
			draw();
	}
}

static void
ls_cfg(void *d, struct zwlr_layer_surface_v1 *l, uint32_t s, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(l, s);
	configured = 1;
	update_input_region();
	if (visible || !opt_autohide) {
		update_brightness();
		update_volume();
		update_battery();
		update_clock();
		draw();
	} else {
		clear_surface();
	}
}

static void
seat_caps(void *d, struct wl_seat *s, uint32_t c)
{
	if ((c & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
		ptr = wl_seat_get_pointer(s);
		wl_pointer_add_listener(ptr, &ptr_l, NULL);
	}
}

static void
reg_glob(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v)
{
	if (!strcmp(i, wl_compositor_interface.name))
		comp = BIND(wl_compositor_interface);
	else if (!strcmp(i, wl_shm_interface.name))
		shm = BIND(wl_shm_interface);
	else if (!strcmp(i, zwlr_layer_shell_v1_interface.name))
		shell = BIND(zwlr_layer_shell_v1_interface);
	else if (!strcmp(i, wl_seat_interface.name))
		wl_seat_add_listener(BIND(wl_seat_interface), &seat_l, NULL);
}

static int
read_file_str(const char *path, char *buf, size_t sz)
{
	int fd, n;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	n = read(fd, buf, sz - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	return 0;
}

static void
setup_mixer(void)
{
	snd_mixer_selem_id_t *sid;

	if (snd_mixer_open(&mixer, 0) < 0)
		return;
	if (snd_mixer_attach(mixer, alsa_device) < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 || snd_mixer_load(mixer) < 0) {
		snd_mixer_close(mixer);
		mixer = NULL;
		return;
	}
	if (snd_mixer_selem_id_malloc(&sid) < 0)
		return;
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, alsa_mixer);
	mixer_elem = snd_mixer_find_selem(mixer, sid);
	snd_mixer_selem_id_free(sid);
}

static int
setup_netlink(void)
{
	struct sockaddr_nl sa;
	int fd;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = 1;
	if ((fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT)) < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void
arm_timer(int enable)
{
	struct itimerspec its;
	struct timespec ts;
	long rem;

	if (timer_fd < 0)
		return;
	memset(&its, 0, sizeof(its));
	if (enable) {
		clock_gettime(CLOCK_REALTIME, &ts);
		rem = 1000000000L - ts.tv_nsec;
		if (rem >= 1000000000L) {
			its.it_value.tv_sec = 1;
			its.it_value.tv_nsec = 0;
		} else if (rem <= 0) {
			its.it_value.tv_sec = 0;
			its.it_value.tv_nsec = 1000000L;
		} else {
			its.it_value.tv_sec = 0;
			its.it_value.tv_nsec = rem;
		}
		its.it_interval.tv_sec = 1;
		its.it_interval.tv_nsec = 0;
	}
	timerfd_settime(timer_fd, 0, &its, NULL);
}

static int
get_brightness(void)
{
	char b[32], path[128];
	int cur, max;

	snprintf(path, sizeof(path), "%s/brightness", backlight_path);
	if (read_file_str(path, b, sizeof(b)) < 0)
		return -1;
	cur = atoi(b);
	snprintf(path, sizeof(path), "%s/max_brightness", backlight_path);
	if (read_file_str(path, b, sizeof(b)) < 0 || cur < 0)
		return -1;
	max = atoi(b);
	if (max <= 0)
		return -1;
	return (cur * 100 + max / 2) / max;
}

static void
set_brightness_delta(int delta)
{
	char b[32], path[128];
	int cur, fd, max, n, step;

	snprintf(path, sizeof(path), "%s/max_brightness", backlight_path);
	if (read_file_str(path, b, sizeof(b)) < 0)
		return;
	max = atoi(b);
	if (max <= 0)
		return;
	snprintf(path, sizeof(path), "%s/brightness", backlight_path);
	if (read_file_str(path, b, sizeof(b)) < 0)
		return;
	cur = atoi(b);
	step = max * 5 / 100;
	if (step < 1)
		step = 1;
	cur += delta * step;
	if (cur < 0)
		cur = 0;
	if (cur > max)
		cur = max;
	if ((fd = open(path, O_WRONLY)) >= 0) {
		n = snprintf(b, sizeof(b), "%d\n", cur);
		if (n > 0)
			write(fd, b, (size_t)n);
		close(fd);
	} else {
		if (delta > 0)
			system("brightnessctl -q set 5%+ >/dev/null 2>&1");
		else
			system("brightnessctl -q set 5%- >/dev/null 2>&1");
	}
}

static int
update_brightness(void)
{
	int br;

	br = get_brightness();
	if (br != cur_brightness) {
		cur_brightness = br;
		return 1;
	}
	return 0;
}

static int
get_volume(int *muted)
{
	long max, min, vol;
	int sw;

	max = 0;
	min = 0;
	vol = 0;
	sw = 1;
	*muted = 0;
	if (!mixer || !mixer_elem)
		return -1;
	snd_mixer_handle_events(mixer);
	snd_mixer_selem_get_playback_volume_range(mixer_elem, &min, &max);
	snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_MONO, &vol);
	snd_mixer_selem_get_playback_switch(mixer_elem, SND_MIXER_SCHN_MONO, &sw);
	*muted = !sw;
	if (max <= min)
		return 0;
	return (vol - min) * 100 / (max - min);
}

static void
set_volume_delta(int delta)
{
	long max, min, step, vol;

	if (!mixer || !mixer_elem)
		return;
	snd_mixer_handle_events(mixer);
	snd_mixer_selem_get_playback_volume_range(mixer_elem, &min, &max);
	snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_MONO, &vol);
	step = (max - min) * 5 / 100;
	if (step < 1)
		step = 1;
	vol += delta * step;
	if (vol < min)
		vol = min;
	if (vol > max)
		vol = max;
	snd_mixer_selem_set_playback_volume_all(mixer_elem, vol);
}

static void
toggle_mute(void)
{
	int sw;

	sw = 1;
	if (!mixer || !mixer_elem)
		return;
	snd_mixer_handle_events(mixer);
	snd_mixer_selem_get_playback_switch(mixer_elem, SND_MIXER_SCHN_MONO, &sw);
	snd_mixer_selem_set_playback_switch_all(mixer_elem, !sw);
}

static int
update_volume(void)
{
	int muted, vol;

	vol = get_volume(&muted);
	if (vol != cur_volume || muted != cur_muted) {
		cur_volume = vol;
		cur_muted = muted;
		return 1;
	}
	return 0;
}

static int
get_battery(int *charging)
{
	char b[32], path[128];
	int cap;

	*charging = 0;
	snprintf(path, sizeof(path), "%s/capacity", battery_path);
	if (read_file_str(path, b, sizeof(b)) < 0)
		return -1;
	cap = atoi(b);
	snprintf(path, sizeof(path), "%s/status", battery_path);
	if (read_file_str(path, b, sizeof(b)) >= 0)
		*charging = (b[0] == 'C');
	return cap;
}

static int
update_battery(void)
{
	int bat, charging;

	bat = get_battery(&charging);
	if (bat != cur_battery || charging != cur_charging) {
		cur_battery = bat;
		cur_charging = charging;
		return 1;
	}
	return 0;
}

static void
get_time(char *buf, size_t sz)
{
	struct tm *tm;
	time_t now;

	now = time(NULL);
	if (!(tm = localtime(&now))) {
		buf[0] = '\0';
		return;
	}
	strftime(buf, sz, time_format, tm);
}

static int
update_clock(void)
{
	char buf[32];

	get_time(buf, sizeof(buf));
	if (strcmp(buf, cur_time) != 0) {
		snprintf(cur_time, sizeof(cur_time), "%s", buf);
		return 1;
	}
	return 0;
}

static void
handle_timer(void)
{
	uint64_t exp;

	if (read(timer_fd, &exp, sizeof(exp)) < 0)
		return;
	if (update_clock() && visible)
		draw();
}

static void
handle_netlink(void)
{
	char buf[2048];
	int dirty;
	ssize_t i, len;

	dirty = 0;
	while ((len = recv(netlink_fd, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[len] = '\0';
		for (i = 0; i < len; i++) {
			if (buf[i] == '\0')
				buf[i] = ' ';
		}
		if (strstr(buf, "backlight"))
			dirty |= update_brightness();
		if (strstr(buf, "power_supply"))
			dirty |= update_battery();
	}
	if (dirty && visible)
		draw();
}

static void
handle_alsa(void)
{
	if (!mixer)
		return;
	snd_mixer_handle_events(mixer);
	if (update_volume() && visible)
		draw();
}

static int
level_brightness(int pct)
{
	if (pct <= 0)
		return 0;
	if (pct >= 100)
		return 6;
	return (pct * 6 + 50) / 100;
}

static int
level_volume(int pct)
{
	int lvl;

	if (pct <= 0)
		return 0;
	if (pct >= 100)
		return 4;
	lvl = (pct * 4 + 50) / 100;
	return lvl < 1 ? 1 : lvl > 4 ? 4 : lvl;
}

static int
level_battery(int pct)
{
	int lvl;

	if (pct <= 0)
		return 0;
	if (pct >= 100)
		return 5;
	lvl = (pct * 6 + 50) / 100;
	if (lvl < 1)
		lvl = 1;
	if (lvl > 6)
		lvl = 6;
	return lvl - 1;
}

static void
rounded_rect(cairo_t *cr, double x0, double y0, double x1, double y1, double r)
{
	double c, max_r;

	max_r = (x1 - x0) / 2.0;
	if ((y1 - y0) / 2.0 < max_r)
		max_r = (y1 - y0) / 2.0;
	if (r > max_r)
		r = max_r;
	if (r <= 0.0) {
		cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
		return;
	}
	c = r * KAPPA;
	cairo_move_to(cr, x0 + r, y0);
	cairo_line_to(cr, x1 - r, y0);
	cairo_curve_to(cr, x1 - r + c, y0, x1, y0 + r - c, x1, y0 + r);
	cairo_line_to(cr, x1, y1 - r);
	cairo_curve_to(cr, x1, y1 - r + c, x1 - r + c, y1, x1 - r, y1);
	cairo_line_to(cr, x0 + r, y1);
	cairo_curve_to(cr, x0 + r - c, y1, x0, y1 - r + c, x0, y1 - r);
	cairo_line_to(cr, x0, y0 + r);
	cairo_curve_to(cr, x0, y0 + r - c, x0 + r - c, y0, x0 + r, y0);
	cairo_close_path(cr);
}

static void
draw_icon_brightness(cairo_t *cr, double x, double y, double sz, int lvl)
{
	double ax, c, k, r, t;

	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, sz, sz);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 0.50, 0.50, 0.34, 0, 2 * PI);
	cairo_set_line_width(cr, 0.085);
	cairo_stroke(cr);

	if (lvl > 0) {
		r = 0.34 - 0.085 / 2.0;
		t = lvl / 6.0;
		ax = r * (2.0 * t - 1.0);
		c = KAPPA * r;
		k = KAPPA * ax;
		cairo_move_to(cr, 0.50, 0.50 - r);
		cairo_curve_to(cr, 0.50 - c, 0.50 - r, 0.50 - r, 0.50 - c, 0.50 - r, 0.50);
		cairo_curve_to(cr, 0.50 - r, 0.50 + c, 0.50 - c, 0.50 + r, 0.50, 0.50 + r);
		cairo_curve_to(cr, 0.50 + k, 0.50 + r, 0.50 + ax, 0.50 + c, 0.50 + ax, 0.50);
		cairo_curve_to(cr, 0.50 + ax, 0.50 - c, 0.50 + k, 0.50 - r, 0.50, 0.50 - r);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

static void
draw_icon_volume(cairo_t *cr, double x, double y, double sz, int lvl, int muted)
{
	double r;
	int i;

	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, sz, sz);

	cairo_move_to(cr, 0.06, 0.38);
	cairo_line_to(cr, 0.21, 0.38);
	cairo_line_to(cr, 0.36, 0.18);
	cairo_line_to(cr, 0.36, 0.82);
	cairo_line_to(cr, 0.21, 0.62);
	cairo_line_to(cr, 0.06, 0.62);
	cairo_close_path(cr);
	cairo_fill(cr);

	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (muted) {
		cairo_set_line_width(cr, 0.08);
		cairo_move_to(cr, 0.52, 0.36);
		cairo_line_to(cr, 0.78, 0.64);
		cairo_move_to(cr, 0.78, 0.36);
		cairo_line_to(cr, 0.52, 0.64);
		cairo_stroke(cr);
	} else {
		for (i = 0; i < 4; i++) {
			r = 0.14 + i * 0.14;
			cairo_set_line_width(cr, i < lvl ? 0.085 : 0.028);
			cairo_new_sub_path(cr);
			cairo_arc(cr, 0.34, 0.50, r, -0.84, 0.84);
			cairo_stroke(cr);
		}
	}
	cairo_restore(cr);
}

static void
draw_icon_battery(cairo_t *cr, double x, double y, double sz, int lvl, int charging)
{
	double filled, fx0, fx1, fy0, fy1;

	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, sz, sz);

	rounded_rect(cr, 0.04, 0.188, 1.07, 0.812, 0.08);
	cairo_set_line_width(cr, 0.08);
	cairo_stroke(cr);

	rounded_rect(cr, 1.12, 0.40, 1.23, 0.60, 0.03);
	cairo_fill(cr);

	fx0 = 0.10;
	fx1 = 1.01;
	fy0 = 0.248;
	fy1 = 0.752;
	filled = fx0 + (fx1 - fx0) * (lvl + 1) / 6.0;

	rounded_rect(cr, fx0, fy0, filled, fy1, 0.03);

	if (charging) {
		cairo_move_to(cr, 0.6705, 0.278);
		cairo_line_to(cr, 0.345, 0.5222);
		cairo_line_to(cr, 0.513, 0.5222);
		cairo_line_to(cr, 0.4605, 0.722);
		cairo_line_to(cr, 0.765, 0.4667);
		cairo_line_to(cr, 0.597, 0.4667);
		cairo_close_path(cr);
		cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	}
	cairo_fill(cr);
	cairo_restore(cr);
}

static void
draw_icon_clock(cairo_t *cr, double x, double y, double sz)
{
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, sz, sz);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width(cr, 0.08);

	cairo_new_sub_path(cr);
	cairo_arc(cr, 0.50, 0.50, 0.34, 0, 2 * PI);
	cairo_stroke(cr);

	cairo_move_to(cr, 0.50, 0.28);
	cairo_line_to(cr, 0.50, 0.50);
	cairo_line_to(cr, 0.66, 0.58);
	cairo_stroke(cr);

	cairo_restore(cr);
}

static void
update_input_region(void)
{
	struct wl_region *r;
	int hx, hy;

	if (!(r = wl_compositor_create_region(comp)))
		return;
	if (visible || !opt_autohide) {
		wl_region_add(r, 0, 0, bar_width, bar_height);
	} else {
		hx = (selected_corner == CORNER_TOP_RIGHT || selected_corner == CORNER_BOTTOM_RIGHT) ? bar_width - hotspot_size : 0;
		hy = (selected_corner == CORNER_BOTTOM_LEFT || selected_corner == CORNER_BOTTOM_RIGHT) ? bar_height - hotspot_size : 0;
		wl_region_add(r, hx, hy, hotspot_size, hotspot_size);
	}
	wl_surface_set_input_region(surf, r);
	wl_region_destroy(r);
}

static void
draw(void)
{
	cairo_font_extents_t fext;
	cairo_text_extents_t text;
	char mod_text[32];
	int icon_y, iw, lvl, mw, x, y;
	size_t i;

	if (!back_surf || !back_cr || !configured)
		return;

	cairo_set_operator(back_cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(back_cr, color_bg[0], color_bg[1], color_bg[2], color_bg[3]);
	cairo_paint(back_cr);

	cairo_set_operator(back_cr, CAIRO_OPERATOR_OVER);

	x = padding_x;
	icon_y = (bar_height - icon_size) / 2;
	if (icon_y < 0)
		icon_y = 0;

	cairo_font_extents(back_cr, &fext);
	y = (bar_height - (int)(fext.height + 0.5)) / 2 + (int)(fext.ascent + 0.5);

	for (i = 0; i < 4; i++) {
		switch (i) {
		case 0:
			cairo_set_source_rgba(back_cr, color_fg[0], color_fg[1], color_fg[2], color_fg[3]);
			lvl = level_brightness(cur_brightness);
			draw_icon_brightness(back_cr, x, icon_y, icon_size, lvl);
			x += icon_size + icon_spacing;
			snprintf(mod_text, sizeof(mod_text), "%d%%", cur_brightness < 0 ? 0 : cur_brightness);
			break;
		case 1:
			if (cur_muted)
				cairo_set_source_rgba(back_cr, color_disabled[0], color_disabled[1], color_disabled[2], color_disabled[3]);
			else
				cairo_set_source_rgba(back_cr, color_fg[0], color_fg[1], color_fg[2], color_fg[3]);
			lvl = level_volume(cur_volume);
			draw_icon_volume(back_cr, x, icon_y, icon_size, lvl, cur_muted);
			x += icon_size + icon_spacing;
			snprintf(mod_text, sizeof(mod_text), "%d%%", cur_volume < 0 ? 0 : cur_volume);
			break;
		case 2:
			cairo_set_source_rgba(back_cr, color_fg[0], color_fg[1], color_fg[2], color_fg[3]);
			lvl = level_battery(cur_battery);
			iw = (int)(BATTERY_WIDTH * icon_size + 0.5);
			draw_icon_battery(back_cr, x, icon_y, icon_size, lvl, cur_charging);
			x += iw + icon_spacing;
			snprintf(mod_text, sizeof(mod_text), "%d%%", cur_battery < 0 ? 0 : cur_battery);
			break;
		case 3:
			cairo_set_source_rgba(back_cr, color_fg[0], color_fg[1], color_fg[2], color_fg[3]);
			draw_icon_clock(back_cr, x, icon_y, icon_size);
			x += icon_size + icon_spacing;
			snprintf(mod_text, sizeof(mod_text), "%s", cur_time);
			break;
		default:
			mod_text[0] = '\0';
			break;
		}

		cairo_text_extents(back_cr, mod_text, &text);
		mw = (int)(text.x_advance + 0.5);
		cairo_move_to(back_cr, x, y);
		cairo_show_text(back_cr, mod_text);
		x += mw + spacing;
		if (i == 0)
			mod_x_bounds[0] = x;
		else if (i == 1)
			mod_x_bounds[1] = x;
	}

	cairo_surface_flush(back_surf);
	memcpy(pixel_data, cairo_image_surface_get_data(back_surf), buffer_size);

	wl_surface_damage(surf, 0, 0, bar_width, bar_height);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
}

static void
clear_surface(void)
{
	if (!pixel_data || !surf || !buf || !configured)
		return;
	memset(pixel_data, 0, buffer_size);
	wl_surface_damage(surf, 0, 0, bar_width, bar_height);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
}

static int
create_shm_buffer(void)
{
	struct wl_shm_pool *pool;
	char name[32];
	int fd;

	buffer_size = (size_t)bar_width * 4 * bar_height;
	snprintf(name, sizeof(name), "/bar-%d", (int)getpid());
	if ((fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600)) < 0) {
		shm_unlink(name);
		if ((fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600)) < 0)
			return -1;
	}
	shm_unlink(name);
	if (ftruncate(fd, buffer_size) < 0 || (pixel_data = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		close(fd);
		return -1;
	}
	pool = wl_shm_create_pool(shm, fd, buffer_size);
	buf = wl_shm_pool_create_buffer(pool, 0, bar_width, bar_height, bar_width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	back_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bar_width, bar_height);
	back_cr = cairo_create(back_surf);
	cairo_select_font_face(back_cr, font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(back_cr, font_size);
	return 0;
}

static void
setup_wayland(void)
{
	struct sigaction sa;
	uint32_t anchor;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	setup_mixer();
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	netlink_fd = setup_netlink();

	if (!(dpy = wl_display_connect(NULL)))
		die("bar: cannot connect to wayland display");
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !shell) {
		cleanup();
		die("bar: missing required wayland globals");
	}
	if (create_shm_buffer() < 0) {
		cleanup();
		die("bar: failed to allocate shm buffer");
	}

	surf = wl_compositor_create_surface(comp);
	ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
	zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
	zwlr_layer_surface_v1_set_size(ls, bar_width, bar_height);

	switch (selected_corner) {
	case CORNER_TOP_LEFT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		break;
	case CORNER_TOP_RIGHT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		break;
	case CORNER_BOTTOM_LEFT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		break;
	case CORNER_BOTTOM_RIGHT:
	default:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		break;
	}
	zwlr_layer_surface_v1_set_anchor(ls, anchor);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, 0);

	wl_surface_commit(surf);
	wl_display_roundtrip(dpy);
}

static void
cleanup(void)
{
	if (timer_fd >= 0)
		close(timer_fd);
	if (netlink_fd >= 0)
		close(netlink_fd);
	if (back_cr)
		cairo_destroy(back_cr);
	if (back_surf)
		cairo_surface_destroy(back_surf);
	if (mixer)
		snd_mixer_close(mixer);
	if (ls)
		zwlr_layer_surface_v1_destroy(ls);
	if (surf)
		wl_surface_destroy(surf);
	if (buf)
		wl_buffer_destroy(buf);
	if (pixel_data && buffer_size)
		munmap(pixel_data, buffer_size);
	if (ptr)
		wl_pointer_destroy(ptr);
	if (comp)
		wl_compositor_destroy(comp);
	if (shm)
		wl_shm_destroy(shm);
	if (shell)
		zwlr_layer_shell_v1_destroy(shell);
	if (dpy)
		wl_display_disconnect(dpy);
}

static void
sig_handler(int sig)
{
	running = 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage: bar [-c corner] [-n] [-v]\n");
	exit(1);
}

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

int
main(int argc, char **argv)
{
	struct pollfd pfds[8];
	int alsa_count, i, nfds, opt, ret;

	alsa_count = 0;
	nfds = 0;
	while ((opt = getopt(argc, argv, "c:nhv")) >= 0) {
		switch (opt) {
		case 'c':
			if (!strcmp(optarg, "tl") || !strcmp(optarg, "0"))
				selected_corner = CORNER_TOP_LEFT;
			else if (!strcmp(optarg, "tr") || !strcmp(optarg, "1"))
				selected_corner = CORNER_TOP_RIGHT;
			else if (!strcmp(optarg, "bl") || !strcmp(optarg, "2"))
				selected_corner = CORNER_BOTTOM_LEFT;
			else if (!strcmp(optarg, "br") || !strcmp(optarg, "3"))
				selected_corner = CORNER_BOTTOM_RIGHT;
			else
				usage();
			break;
		case 'n':
			opt_autohide = 0;
			visible = 1;
			break;
		case 'v':
			puts("bar-" VERSION);
			return 0;
		case 'h':
		default:
			usage();
		}
	}

	if (!opt_autohide)
		visible = 1;

	setup_wayland();

	if (timer_fd >= 0 && visible)
		arm_timer(1);

	update_brightness();
	update_volume();
	update_battery();
	update_clock();

	pfds[0].fd = wl_display_get_fd(dpy);
	pfds[0].events = POLLIN;
	pfds[1].fd = timer_fd;
	pfds[1].events = POLLIN;
	pfds[2].fd = netlink_fd;
	pfds[2].events = POLLIN;
	nfds = 3;

	if (mixer) {
		alsa_count = snd_mixer_poll_descriptors_count(mixer);
		if (alsa_count > 0 && alsa_count <= 5) {
			snd_mixer_poll_descriptors(mixer, &pfds[3], alsa_count);
			nfds += alsa_count;
		} else {
			alsa_count = 0;
		}
	}

	while (running) {
		while (wl_display_prepare_read(dpy) < 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		ret = poll(pfds, nfds, -1);
		if (ret < 0) {
			wl_display_cancel_read(dpy);
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP))
			wl_display_read_events(dpy);
		else
			wl_display_cancel_read(dpy);

		wl_display_dispatch_pending(dpy);

		if (timer_fd >= 0 && (pfds[1].revents & POLLIN))
			handle_timer();

		if (netlink_fd >= 0 && (pfds[2].revents & POLLIN))
			handle_netlink();

		if (mixer && alsa_count > 0) {
			for (i = 0; i < alsa_count; i++) {
				if (pfds[3 + i].revents) {
					handle_alsa();
					break;
				}
			}
		}
	}

	cleanup();
	return 0;
}
