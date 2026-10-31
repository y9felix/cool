#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
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

#include "font.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define PI 3.14159265358979323846
#define LEVEL(v, m) ((v) <= 0 ? 0 : (v) >= 100 ? (m) : ((v) * (m) + 50) / 100)
#define BIND(iface) wl_registry_bind(r, n, &(iface), 1)

enum {
	CORNER_TOP_LEFT,
	CORNER_TOP_RIGHT,
	CORNER_BOTTOM_LEFT,
	CORNER_BOTTOM_RIGHT
};

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
static snd_mixer_t *mixer;
static snd_mixer_elem_t *mixer_elem;
static void *pixel_data;
static size_t buffer_size;
static int timer_fd = -1, netlink_fd = -1, cur_bri = -1, cur_vol = -1;
static int cur_mute = -1, cur_bat = -1, cur_chg = -1, selected_corner = corner;
static int opt_autohide = autohide, visible, configured, pointer_x, bounds[2];
static char cur_time[32];
static volatile sig_atomic_t running = 1;

static void
die(const char *msg)
{
	fprintf(stderr, "bar: %s\n", msg);
	exit(1);
}

static int
read_sysfs(const char *base, const char *entry, char *out, size_t sz)
{
	char b[32], p[128];
	int fd, n;

	snprintf(p, sizeof(p), "%s/%s", base, entry);
	if ((fd = open(p, O_RDONLY)) < 0)
		return -1;
	n = read(fd, out ? out : b, (out ? sz : sizeof(b)) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	if (out) {
		out[n] = '\0';
		return 0;
	}
	b[n] = '\0';
	return atoi(b);
}

static int
update_brightness(void)
{
	int cur, max, val;

	cur = read_sysfs(backlight_path, "brightness", NULL, 0);
	max = read_sysfs(backlight_path, "max_brightness", NULL, 0);
	val = (cur < 0 || max <= 0) ? -1 : (cur * 100 + max / 2) / max;
	if (val == cur_bri)
		return 0;
	cur_bri = val;
	return 1;
}

static void
setup_mixer(void)
{
	snd_mixer_selem_id_t *sid;

	if (snd_mixer_open(&mixer, 0) < 0)
		return;
	if (snd_mixer_attach(mixer, alsa_device) < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(mixer) < 0 || snd_mixer_selem_id_malloc(&sid) < 0) {
		snd_mixer_close(mixer);
		mixer = NULL;
		return;
	}
	snd_mixer_selem_id_set_name(sid, alsa_mixer);
	mixer_elem = snd_mixer_find_selem(mixer, sid);
	snd_mixer_selem_id_free(sid);
}

static void
toggle_mute(void)
{
	int sw;

	if (mixer_elem) {
		snd_mixer_selem_get_playback_switch(mixer_elem, SND_MIXER_SCHN_MONO, &sw);
		snd_mixer_selem_set_playback_switch_all(mixer_elem, !sw);
	}
}

static int
update_volume(void)
{
	long min, max, vol;
	int muted, sw = 1, v;

	if (!mixer_elem)
		return 0;
	snd_mixer_selem_get_playback_volume_range(mixer_elem, &min, &max);
	snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_MONO, &vol);
	snd_mixer_selem_get_playback_switch(mixer_elem, SND_MIXER_SCHN_MONO, &sw);
	muted = !sw;
	v = max > min ? (vol - min) * 100 / (max - min) : 0;
	if (v == cur_vol && muted == cur_mute)
		return 0;
	cur_vol = v;
	cur_mute = muted;
	return 1;
}

static int
update_battery(void)
{
	char b[16];
	int bat, chg;

	bat = read_sysfs(battery_path, "capacity", NULL, 0);
	chg = (read_sysfs(battery_path, "status", b, sizeof(b)) == 0 && b[0] == 'C');
	if (bat == cur_bat && chg == cur_chg)
		return 0;
	cur_bat = bat;
	cur_chg = chg;
	return 1;
}

static int
update_clock(void)
{
	time_t now = time(NULL);
	char b[32];

	strftime(b, sizeof(b), time_format, localtime(&now));
	if (!strcmp(b, cur_time))
		return 0;
	strcpy(cur_time, b);
	return 1;
}

static void
update_all(void)
{
	update_brightness();
	update_volume();
	update_battery();
	update_clock();
}

static void
blend_pixel(int x, int y, uint32_t c, uint8_t a)
{
	uint32_t *p, bg, fg_a, inv_a, r, g, b, out_a;

	if (x < 0 || x >= bar_width || y < 0 || y >= bar_height || !a)
		return;
	p = (uint32_t *)pixel_data + y * bar_width + x;
	fg_a = (((c >> 24) & 0xff) * a) / 255;
	if (!fg_a)
		return;
	if (fg_a >= 255) {
		*p = c;
		return;
	}
	bg = *p;
	inv_a = 255 - fg_a;
	out_a = fg_a + (((bg >> 24) & 0xff) * inv_a) / 255;
	r = (((c >> 16) & 0xff) * fg_a + ((bg >> 16) & 0xff) * inv_a) / 255;
	g = (((c >> 8) & 0xff) * fg_a + ((bg >> 8) & 0xff) * inv_a) / 255;
	b = ((c & 0xff) * fg_a + (bg & 0xff) * inv_a) / 255;
	*p = (out_a << 24) | (r << 16) | (g << 8) | b;
}

static int
draw_string(int x, int y, const char *str, uint32_t color)
{
	int orig_x = x;

	while (*str) {
		int idx = ((unsigned char)*str >= 32 && (unsigned char)*str <= 126) ? ((unsigned char)*str - 32) : 0;
		int row, col;

		for (row = 0; row < FONT_HEIGHT; row++) {
			for (col = 0; col < FONT_WIDTH; col++) {
				uint8_t a = font_data[idx][row * FONT_WIDTH + col];
				if (a)
					blend_pixel(x + col, y + row, color, a);
			}
		}
		x += FONT_WIDTH;
		str++;
	}
	return x - orig_x;
}

static void
draw_line(double x0, double y0, double x1, double y1, uint32_t color, double th)
{
	int min_x = (int)(floor(fmin(x0, x1) - th - 1.0));
	int max_x = (int)(ceil(fmax(x0, x1) + th + 1.0));
	int min_y = (int)(floor(fmin(y0, y1) - th - 1.0));
	int max_y = (int)(ceil(fmax(y0, y1) + th + 1.0));
	double dx = x1 - x0, dy = y1 - y0;
	double len_sq = dx * dx + dy * dy;
	int x, y;

	for (y = min_y; y <= max_y; y++) {
		for (x = min_x; x <= max_x; x++) {
			double t, px, py, d, a;

			t = len_sq <= 0.0001 ? 0.0 : ((x + 0.5 - x0) * dx + (y + 0.5 - y0) * dy) / len_sq;
			if (t < 0.0)
				t = 0.0;
			else if (t > 1.0)
				t = 1.0;
			px = x0 + t * dx;
			py = y0 + t * dy;
			d = hypot(x + 0.5 - px, y + 0.5 - py);
			if (d < th / 2.0 + 0.5) {
				a = 1.0 - (d - (th / 2.0 - 0.5));
				if (a > 0.0)
					blend_pixel(x, y, color, (uint8_t)((a > 1.0 ? 1.0 : a) * 255.0 + 0.5));
			}
		}
	}
}

static void
draw_arc(double cx, double cy, double r, double a0, double a1, uint32_t color, double th)
{
	int min_x = (int)(floor(cx - r - th - 1.0));
	int max_x = (int)(ceil(cx + r + th + 1.0));
	int min_y = (int)(floor(cy - r - th - 1.0));
	int max_y = (int)(ceil(cy + r + th + 1.0));
	int x, y;

	for (y = min_y; y <= max_y; y++) {
		for (x = min_x; x <= max_x; x++) {
			double dx = x + 0.5 - cx;
			double dy = y + 0.5 - cy;
			double d = hypot(dx, dy);
			double a = atan2(dy, dx);

			if ((a0 <= -PI && a1 >= PI) || (a >= a0 && a <= a1)) {
				double dist = fabs(d - r);

				if (dist < th / 2.0 + 0.5) {
					double cov = 1.0 - (dist - (th / 2.0 - 0.5));

					if (cov > 0.0)
						blend_pixel(x, y, color, (uint8_t)((cov > 1.0 ? 1.0 : cov) * 255.0 + 0.5));
				}
			}
		}
	}
}

static void
draw_rounded_rect(double x0, double y0, double w, double h, double r, uint32_t color, double th, int fill)
{
	int min_x = (int)(floor(x0 - th - 1.0));
	int max_x = (int)(ceil(x0 + w + th + 1.0));
	int min_y = (int)(floor(y0 - th - 1.0));
	int max_y = (int)(ceil(y0 + h + th + 1.0));
	int x, y;

	for (y = min_y; y <= max_y; y++) {
		for (x = min_x; x <= max_x; x++) {
			double px = (x + 0.5) - (x0 + w / 2.0);
			double py = (y + 0.5) - (y0 + h / 2.0);
			double qx = fabs(px) - (w / 2.0 - r);
			double qy = fabs(py) - (h / 2.0 - r);
			double mx = qx > 0.0 ? qx : 0.0;
			double my = qy > 0.0 ? qy : 0.0;
			double inner = (qx > qy ? qx : qy);
			double d = sqrt(mx * mx + my * my) + (inner > 0.0 ? 0.0 : inner) - r;
			double cov = fill ? (d < 0.5 ? 0.5 - d : 0.0)
			                  : (fabs(d) < th / 2.0 + 0.5 ? 1.0 - (fabs(d) - (th / 2.0 - 0.5)) : 0.0);

			if (cov > 0.0)
				blend_pixel(x, y, color, (uint8_t)((cov > 1.0 ? 1.0 : cov) * 255.0 + 0.5));
		}
	}
}

static int
point_in_poly(double px, double py, double pts[][2], int n)
{
	int i, j, c = 0;

	for (i = 0, j = n - 1; i < n; j = i++) {
		if (((pts[i][1] > py) != (pts[j][1] > py)) &&
		    (px < (pts[j][0] - pts[i][0]) * (py - pts[i][1]) / (pts[j][1] - pts[i][1]) + pts[i][0]))
			c = !c;
	}
	return c;
}

static void
draw_poly(double pts[][2], int n, uint32_t color)
{
	double min_x = pts[0][0], max_x = pts[0][0], min_y = pts[0][1], max_y = pts[0][1];
	int i, x, y;

	for (i = 1; i < n; i++) {
		if (pts[i][0] < min_x)
			min_x = pts[i][0];
		if (pts[i][0] > max_x)
			max_x = pts[i][0];
		if (pts[i][1] < min_y)
			min_y = pts[i][1];
		if (pts[i][1] > max_y)
			max_y = pts[i][1];
	}
	for (y = (int)floor(min_y); y <= (int)ceil(max_y); y++) {
		for (x = (int)floor(min_x); x <= (int)ceil(max_x); x++) {
			int hits = point_in_poly(x + 0.25, y + 0.25, pts, n)
			         + point_in_poly(x + 0.75, y + 0.25, pts, n)
			         + point_in_poly(x + 0.25, y + 0.75, pts, n)
			         + point_in_poly(x + 0.75, y + 0.75, pts, n);

			if (hits > 0)
				blend_pixel(x, y, color, (uint8_t)(hits * 63.75 + 0.5));
		}
	}
}

static void
draw_icon_brightness(int x, int y, int lvl, uint32_t color)
{
	double cx = x + 10.0, cy = y + 10.0, r = 5.95;
	double ax = r * ((double)lvl / 3.0 - 1.0);
	int px, py;

	draw_arc(cx, cy, 6.8, -PI, PI, color, 1.7);
	if (lvl <= 0)
		return;
	for (py = (int)floor(cy - r - 1.0); py <= (int)ceil(cy + r + 1.0); py++) {
		for (px = (int)floor(cx - r - 1.0); px <= (int)ceil(cx + r + 1.0); px++) {
			double dx = px + 0.5 - cx;
			double dy = py + 0.5 - cy;

			if (fabs(dy) < r) {
				double w = sqrt(1.0 - (dy / r) * (dy / r));

				if (dx >= -r * w && dx <= ax * w) {
					double d = hypot(dx, dy);
					double cov = 1.0;

					if (d > r - 0.5)
						cov = 1.0 - (d - (r - 0.5));
					if (cov > 1.0)
						cov = 1.0;
					if (cov > 0.0)
						blend_pixel(px, py, color, (uint8_t)(cov * 255.0 + 0.5));
				}
			}
		}
	}
}

static void
draw_icon_volume(int x, int y, int lvl, int muted, uint32_t color)
{
	double pts[6][2] = {
		{ x + 1.2, y + 7.6 }, { x + 4.2, y + 7.6 }, { x + 7.2, y + 3.6 },
		{ x + 7.2, y + 16.4 }, { x + 4.2, y + 12.4 }, { x + 1.2, y + 12.4 }
	};
	int i;

	draw_poly(pts, 6, color);
	if (muted) {
		draw_line(x + 10.4, y + 7.2, x + 15.6, y + 12.8, color, 1.6);
		draw_line(x + 15.6, y + 7.2, x + 10.4, y + 12.8, color, 1.6);
	} else {
		for (i = 0; i < 4; i++) {
			double r = (0.14 + i * 0.14) * 20.0;
			double th = (i < lvl ? 1.7 : 0.6);

			draw_arc(x + 6.8, y + 10.0, r, -0.84, 0.84, color, th);
		}
	}
}

static void
draw_icon_battery(int x, int y, int lvl, int chg, uint32_t color)
{
	double filled_w = 2.0 + (18.2 * (lvl + 1)) / 6.0;
	double bolt[6][2] = {
		{ x + 13.41, y + 5.56 }, { x + 6.90, y + 10.44 }, { x + 10.26, y + 10.44 },
		{ x + 9.21, y + 14.44 }, { x + 15.30, y + 9.33 }, { x + 11.94, y + 9.33 }
	};

	draw_rounded_rect(x + 0.8, y + 3.8, 20.6, 12.4, 1.6, color, 1.6, 0);
	draw_rounded_rect(x + 22.4, y + 8.0, 2.2, 4.0, 0.6, color, 0, 1);
	draw_rounded_rect(x + 2.0, y + 5.0, filled_w, 10.0, 0.6, color, 0, 1);
	if (chg)
		draw_poly(bolt, 6, color_bg);
}

static void
draw_icon_clock(int x, int y, uint32_t color)
{
	draw_arc(x + 10.0, y + 10.0, 6.8, -PI, PI, color, 1.6);
	draw_line(x + 10.0, y + 10.0, x + 10.0, y + 5.6, color, 1.6);
	draw_line(x + 10.0, y + 10.0, x + 13.2, y + 11.6, color, 1.6);
}

static void
update_input_region(void)
{
	struct wl_region *r;
	int x = 0, y = 0, w = bar_width, h = bar_height;

	if (!(r = wl_compositor_create_region(comp)))
		return;
	if (!visible && opt_autohide) {
		w = h = hotspot_size;
		x = (selected_corner & 1) ? bar_width - hotspot_size : 0;
		y = (selected_corner & 2) ? bar_height - hotspot_size : 0;
	}
	wl_region_add(r, x, y, w, h);
	wl_surface_set_input_region(surf, r);
	wl_region_destroy(r);
}

static void
draw(void)
{
	char mod_text[32];
	uint32_t color, *p;
	int icon_y, iw, text_y, x;
	size_t i;

	if (!configured || !pixel_data)
		return;
	if (opt_autohide && !visible) {
		memset(pixel_data, 0, buffer_size);
	} else {
		p = (uint32_t *)pixel_data;
		for (i = 0; i < (size_t)bar_width * bar_height; i++)
			p[i] = color_bg;

		x = padding_x;
		icon_y = (bar_height - icon_size) / 2;
		if (icon_y < 0)
			icon_y = 0;
		text_y = (bar_height - FONT_HEIGHT) / 2;

		for (i = 0; i < 4; i++) {
			color = color_fg;
			iw = icon_size;
			switch (i) {
			case 0:
				draw_icon_brightness(x, icon_y, LEVEL(cur_bri, 6), color);
				snprintf(mod_text, sizeof(mod_text), "%d%%", cur_bri < 0 ? 0 : cur_bri);
				break;
			case 1:
				if (cur_mute)
					color = color_disabled;
				draw_icon_volume(x, icon_y, LEVEL(cur_vol, 4), cur_mute, color);
				snprintf(mod_text, sizeof(mod_text), "%d%%", cur_vol < 0 ? 0 : cur_vol);
				break;
			case 2:
				iw = (int)(1.25 * icon_size + 0.5);
				draw_icon_battery(x, icon_y, LEVEL(cur_bat, 5), cur_chg, color);
				snprintf(mod_text, sizeof(mod_text), "%d%%", cur_bat < 0 ? 0 : cur_bat);
				break;
			case 3:
				draw_icon_clock(x, icon_y, color);
				snprintf(mod_text, sizeof(mod_text), "%s", cur_time);
				break;
			}
			x += iw + icon_spacing;
			x += draw_string(x, text_y, mod_text, color) + spacing;
			if (i < 2)
				bounds[i] = x;
		}
	}
	wl_surface_damage(surf, 0, 0, bar_width, bar_height);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
}

static void
arm_timer(int enable)
{
	struct itimerspec its = {{0, 0}, {0, 0}};
	struct timespec ts;

	if (timer_fd >= 0 && enable) {
		clock_gettime(CLOCK_REALTIME, &ts);
		its.it_value.tv_nsec = 1000000000L - ts.tv_nsec;
		its.it_interval.tv_sec = 1;
	}
	timerfd_settime(timer_fd, 0, &its, NULL);
}

static void
ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t sx, wl_fixed_t sy)
{
	pointer_x = wl_fixed_to_int(sx);
	if (!visible) {
		visible = 1;
		arm_timer(1);
		update_input_region();
		update_all();
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
		draw();
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
	if (st && b == 0x110 && pointer_x >= bounds[0] && pointer_x < bounds[1]) {
		toggle_mute();
		if (update_volume() && visible)
			draw();
	}
}

static void
noop(void *d, struct wl_pointer *p, uint32_t a, uint32_t b, wl_fixed_t c)
{
}

static void
ls_cfg(void *d, struct zwlr_layer_surface_v1 *l, uint32_t s, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(l, s);
	configured = 1;
	update_input_region();
	if (visible || !opt_autohide)
		update_all();
	draw();
}

static void
ls_closed(void *d, struct zwlr_layer_surface_v1 *l)
{
	running = 0;
}

static const struct wl_pointer_listener ptr_l = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_btn, .axis = noop
};
static const struct zwlr_layer_surface_v1_listener ls_l = { .configure = ls_cfg, .closed = ls_closed };

static void
seat_caps(void *d, struct wl_seat *s, uint32_t c)
{
	if ((c & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
		ptr = wl_seat_get_pointer(s);
		wl_pointer_add_listener(ptr, &ptr_l, NULL);
	}
}

static const struct wl_seat_listener seat_l = { .capabilities = seat_caps };

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

static const struct wl_registry_listener reg_l = { .global = reg_glob };

static int
create_shm_buffer(void)
{
	struct wl_shm_pool *pool;
	char name[24];
	int fd;

	buffer_size = (size_t)bar_width * 4 * bar_height;
	snprintf(name, sizeof(name), "/bar%d", (int)getpid());
	if ((fd = shm_open(name, O_CREAT | O_RDWR, 0600)) < 0)
		return -1;
	shm_unlink(name);
	if (ftruncate(fd, buffer_size) < 0 ||
	    (pixel_data = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		close(fd);
		return -1;
	}
	pool = wl_shm_create_pool(shm, fd, buffer_size);
	buf = wl_shm_pool_create_buffer(pool, 0, bar_width, bar_height, bar_width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return 0;
}

static void
sig_handler(int sig)
{
	running = 0;
}

static void
setup_wayland(void)
{
	static const uint32_t anchors[] = { 5, 9, 6, 10 };
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	struct sigaction act = { .sa_handler = sig_handler };

	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	setup_mixer();
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if ((netlink_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT)) >= 0) {
		if (bind(netlink_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(netlink_fd);
			netlink_fd = -1;
		}
	}

	if (!(dpy = wl_display_connect(NULL)))
		die("cannot connect to wayland display");
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !shell || create_shm_buffer() < 0)
		die("missing wayland globals or shm failure");

	surf = wl_compositor_create_surface(comp);
	ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
	zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
	zwlr_layer_surface_v1_set_size(ls, bar_width, bar_height);
	zwlr_layer_surface_v1_set_anchor(ls, anchors[selected_corner & 3]);
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
	if (mixer)
		snd_mixer_close(mixer);
	if (ls)
		zwlr_layer_surface_v1_destroy(ls);
	if (dpy)
		wl_display_disconnect(dpy);
}

int
main(int argc, char **argv)
{
	struct pollfd pfds[8];
	int alsa_count = 0, i, nfds, opt;

	while ((opt = getopt(argc, argv, "c:nhv")) >= 0) {
		switch (opt) {
		case 'c':
			if (optarg[0] >= '0' && optarg[0] <= '3')
				selected_corner = optarg[0] - '0';
			else
				selected_corner = ((optarg[0] == 'b') << 1) | (optarg[1] == 'r');
			break;
		case 'n':
			opt_autohide = 0;
			visible = 1;
			break;
		case 'v':
			puts("bar-" VERSION);
			return 0;
		default:
			fprintf(stderr, "usage: bar [-c corner] [-n] [-v]\n");
			return 1;
		}
	}

	setup_wayland();
	if (timer_fd >= 0 && visible)
		arm_timer(1);
	update_all();

	pfds[0] = (struct pollfd){ wl_display_get_fd(dpy), POLLIN, 0 };
	pfds[1] = (struct pollfd){ timer_fd, POLLIN, 0 };
	pfds[2] = (struct pollfd){ netlink_fd, POLLIN, 0 };
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
		uint64_t exp;
		char nlk[1024];
		int dirty = 0;

		while (wl_display_prepare_read(dpy) < 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		if (poll(pfds, nfds, -1) < 0) {
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

		if (timer_fd >= 0 && (pfds[1].revents & POLLIN)) {
			if (read(timer_fd, &exp, sizeof(exp)) > 0)
				dirty |= update_clock();
		}
		if (netlink_fd >= 0 && (pfds[2].revents & POLLIN)) {
			ssize_t j, len;

			while ((len = recv(netlink_fd, nlk, sizeof(nlk) - 1, 0)) > 0) {
				for (j = 0; j < len; j++) {
					if (nlk[j] == '\0')
						nlk[j] = ' ';
				}
				nlk[len] = '\0';
				if (strstr(nlk, "backlight"))
					dirty |= update_brightness();
				if (strstr(nlk, "power_supply"))
					dirty |= update_battery();
			}
		}
		if (mixer && alsa_count > 0) {
			for (i = 0; i < alsa_count; i++) {
				if (pfds[3 + i].revents) {
					snd_mixer_handle_events(mixer);
					dirty |= update_volume();
					break;
				}
			}
		}
		if (dirty && visible)
			draw();
	}

	cleanup();
	return 0;
}
