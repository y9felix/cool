#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <math.h>
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

#include "font.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef VERSION
#define VERSION "1.0"
#endif

enum {
	CORNER_TOP_LEFT,
	CORNER_TOP_RIGHT,
	CORNER_BOTTOM_LEFT,
	CORNER_BOTTOM_RIGHT
};

static void arm_timer(int enable);
static void blend_pixel(int x, int y, uint32_t c, int a);
static double box_dist(double px, double py, double cx, double cy, double rx, double ry, double r);
static int check_alsa(struct pollfd *pfds, int count);
static void cleanup(void);
static void die(const char *fmt, ...);
static void draw(void);
static void draw_arc(double cx, double cy, double r, int arc, uint32_t c, double th);
static void draw_arc_pixel(int x, int y, double cx, double cy, double r, int arc, uint32_t c, double th);
static void draw_backlight(int x, int y, uint32_t c);
static void draw_backlight_pixel(int px, int py, int x, int y, double ax, double w, uint32_t c);
static void draw_battery(int x, int y, uint32_t c);
static void draw_battery_pixel(int px, int py, int x, int y, double bx1, uint32_t c);
static void draw_char(int x, int y, int ch, uint32_t c);
static void draw_clock(int x, int y, uint32_t c);
static void draw_line(double x0, double y0, double x1, double y1, uint32_t c, double th);
static int draw_string(int x, int y, const char *s, uint32_t c);
static void draw_volume(int x, int y, uint32_t c);
static void ls_cfg(void *d, struct zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h);
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *s);
static void noop(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, wl_fixed_t v);
static int point_in_poly(double px, double py, const double pts[][2], int n);
static void ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st);
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy);
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf);
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy);
static int read_sysfs(const char *base, const char *entry);
static void reg_glob(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v);
static void seat_caps(void *d, struct wl_seat *s, uint32_t c);
static void set_visibility(int v);
static void setup(void);
static int setup_alsa_poll(struct pollfd *pfds);
static void setup_mixer(void);
static void sig_handler(int sig);
static void update_all(void);
static int update_clock(void);
static void update_input_region(void);
static int update_sysfs(void);
static int update_volume(void);
static void usage(void);

#include "config.h"

const struct wl_interface xdg_popup_interface;

static const uint32_t anchors[] = {
	ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
};
static const double bolt[6][2] = {
	{ 13.41,  5.56 }, {  6.90, 10.44 }, { 10.26, 10.44 },
	{  9.21, 14.44 }, { 15.30,  9.33 }, { 11.94,  9.33 }
};
static const double spk[6][2] = {
	{ 1.2,  7.6 }, { 4.2,  7.6 }, { 7.2,  3.6 },
	{ 7.2, 16.4 }, { 4.2, 12.4 }, { 1.2, 12.4 }
};

static const struct wl_pointer_listener ptr_l = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_btn,
	.axis = noop
};
static const struct zwlr_layer_surface_v1_listener ls_l = {
	.configure = ls_cfg,
	.closed = ls_closed
};
static const struct wl_seat_listener seat_l = {
	.capabilities = seat_caps
};
static const struct wl_registry_listener reg_l = {
	.global = reg_glob
};

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
static int timer_fd = -1;
static int netlink_fd = -1;
static int cur_bright = -1;
static int cur_vol = -1;
static int cur_bat = -1;
static int cur_mute = -1;
static int cur_chg = -1;
static int selected_corner;
static int opt_autohide;
static int visible;
static int configured;
static char cur_time[32];
static volatile sig_atomic_t running = 1;

static void
arm_timer(int enable)
{
	struct itimerspec its;
	struct timespec ts;

	if (timer_fd < 0)
		return;
	memset(&its, 0, sizeof(its));
	if (enable) {
		clock_gettime(CLOCK_REALTIME, &ts);
		its.it_value.tv_nsec = 1000000000L - ts.tv_nsec;
		its.it_interval.tv_sec = 1;
	}
	timerfd_settime(timer_fd, 0, &its, NULL);
}

static void
blend_pixel(int x, int y, uint32_t c, int a)
{
	uint32_t *p, bg, fa, ia;

	if (x < 0 || x >= bar_width || y < 0 || y >= bar_height || a <= 0)
		return;
	p = (uint32_t *)pixel_data + y * bar_width + x;
	if ((fa = (((c >> 24) & 0xff) * (a > 255 ? 255 : a)) / 255) >= 255) {
		*p = c;
		return;
	}
	if (!fa)
		return;
	bg = *p;
	ia = 255 - fa;
	*p = ((fa + (((bg >> 24) & 0xff) * ia) / 255) << 24) | (((((c >> 16) & 0xff) * fa + ((bg >> 16) & 0xff) * ia) / 255) << 16) | (((((c >> 8) & 0xff) * fa + ((bg >> 8) & 0xff) * ia) / 255) << 8) | (((c & 0xff) * fa + (bg & 0xff) * ia) / 255);
}

static double
box_dist(double px, double py, double cx, double cy, double rx, double ry, double r)
{
	double mx, my, qx, qy;

	qx = fabs(px - cx) - rx;
	qy = fabs(py - cy) - ry;
	mx = qx > 0.0 ? qx : 0.0;
	my = qy > 0.0 ? qy : 0.0;
	return sqrt(mx * mx + my * my) + (qx > qy ? (qx < 0.0 ? qx : 0.0) : (qy < 0.0 ? qy : 0.0)) - r;
}

static int
check_alsa(struct pollfd *pfds, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (!pfds[i].revents)
			continue;
		if (snd_mixer_handle_events(mixer) < 0)
			setup_mixer();
		return update_volume();
	}
	return 0;
}

static void
cleanup(void)
{
	if (mixer)
		snd_mixer_close(mixer);
	if (timer_fd >= 0)
		close(timer_fd);
	if (netlink_fd >= 0)
		close(netlink_fd);
	if (ptr)
		wl_pointer_destroy(ptr);
	if (buf)
		wl_buffer_destroy(buf);
	if (ls)
		zwlr_layer_surface_v1_destroy(ls);
	if (surf)
		wl_surface_destroy(surf);
	if (pixel_data)
		munmap(pixel_data, (size_t)bar_width * 4 * bar_height);
	if (dpy)
		wl_display_disconnect(dpy);
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

static void
draw(void)
{
	char s[32];
	uint32_t c, *p;
	int i, icon_y, text_y, x;

	if (!configured || !pixel_data)
		return;
	if (opt_autohide && !visible) {
		memset(pixel_data, 0, (size_t)bar_width * 4 * bar_height);
	} else {
		p = (uint32_t *)pixel_data;
		for (i = 0; i < bar_width * bar_height; i++)
			p[i] = color_bg;
		x = padding_x;
		if ((icon_y = (bar_height - icon_size) / 2) < 0)
			icon_y = 0;
		text_y = (bar_height - FONT_HEIGHT) / 2;

		draw_backlight(x, icon_y, color_fg);
		x += icon_size + icon_spacing;
		snprintf(s, sizeof(s), "%d%%", cur_bright);
		x += draw_string(x, text_y, s, color_fg) + spacing;

		c = cur_mute ? color_disabled : color_fg;
		draw_volume(x, icon_y, c);
		x += icon_size + icon_spacing;
		snprintf(s, sizeof(s), "%d%%", cur_vol);
		x += draw_string(x, text_y, s, c) + spacing;

		draw_battery(x, icon_y, color_fg);
		x += (int)(1.25 * icon_size + 0.5) + icon_spacing;
		snprintf(s, sizeof(s), "%d%%", cur_bat);
		x += draw_string(x, text_y, s, color_fg) + spacing;

		draw_clock(x, icon_y, color_fg);
		x += icon_size + icon_spacing;
		draw_string(x, text_y, cur_time, color_fg);
	}
	wl_surface_damage(surf, 0, 0, bar_width, bar_height);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
}

static void
draw_arc(double cx, double cy, double r, int arc, uint32_t c, double th)
{
	int x, y;

	for (y = (int)(cy - r - th); y <= (int)(cy + r + th + 1.0); y++)
		for (x = (int)(cx - r - th); x <= (int)(cx + r + th + 1.0); x++)
			draw_arc_pixel(x, y, cx, cy, r, arc, c, th);
}

static void
draw_arc_pixel(int x, int y, double cx, double cy, double r, int arc, uint32_t c, double th)
{
	double d, dx, dy;

	dx = x + 0.5 - cx;
	dy = y + 0.5 - cy;
	d = fabs(hypot(dx, dy) - r);
	if (d >= th / 2.0 + 0.5 || (arc && (dx <= 0.0 || fabs(dy) > 1.1155 * dx)))
		return;
	blend_pixel(x, y, c, (int)((1.0 - (d - (th / 2.0 - 0.5))) * 255.0));
}

static void
draw_backlight(int x, int y, uint32_t c)
{
	double ax, dy, w;
	int lvl, px, py;

	draw_arc(x + 10.0, y + 10.0, 6.8, 0, c, 1.7);
	lvl = cur_bright <= 0 ? 0 : (cur_bright >= 100 ? 6 : (cur_bright * 6 + 50) / 100);
	if (lvl <= 0)
		return;
	ax = 5.95 * ((double)lvl / 3.0 - 1.0);
	for (py = y + 3; py <= y + 17; py++) {
		dy = py + 0.5 - (y + 10.0);
		if (fabs(dy) >= 5.95)
			continue;
		w = sqrt(1.0 - (dy / 5.95) * (dy / 5.95));
		for (px = x + 3; px <= x + 17; px++)
			draw_backlight_pixel(px, py, x, y, ax, w, c);
	}
}

static void
draw_backlight_pixel(int px, int py, int x, int y, double ax, double w, uint32_t c)
{
	double d, dx, dy;

	dx = px + 0.5 - (x + 10.0);
	dy = py + 0.5 - (y + 10.0);
	if (dx < -5.95 * w || dx > ax * w)
		return;
	d = hypot(dx, dy);
	blend_pixel(px, py, c, (int)((d > 5.45 ? 1.0 - (d - 5.45) : 1.0) * 255.0));
}

static void
draw_battery(int x, int y, uint32_t c)
{
	double bw, bx1;
	int px, py;

	bw = cur_bat <= 0 ? 0.0 : (18.2 * (cur_bat > 100 ? 100 : cur_bat)) / 100.0;
	bx1 = x + 2.0 + bw;
	for (py = y + 2; py <= y + 18; py++)
		for (px = x; px <= x + 25; px++)
			draw_battery_pixel(px, py, x, y, bx1, c);
}

static void
draw_battery_pixel(int px, int py, int x, int y, double bx1, uint32_t c)
{
	double d;
	int hits;

	d = box_dist(px + 0.5, py + 0.5, x + 11.1, y + 10.0, 8.7, 4.6, 1.6);
	blend_pixel(px, py, c, (int)((1.0 - (fabs(d) - 0.3)) * 255.0));

	d = box_dist(px + 0.5, py + 0.5, x + 23.5, y + 10.0, 0.5, 1.4, 0.6);
	blend_pixel(px, py, c, (int)((0.5 - d) * 255.0));

	if (px < x + 2 || px > x + 20 || py < y + 5 || py > y + 14 || (bx1 <= x + 2.0 && !cur_chg))
		return;
	hits = ((px + 0.25 <= bx1) ^ (cur_chg && point_in_poly(px + 0.25 - x, py + 0.25 - y, bolt, 6))) + ((px + 0.75 <= bx1) ^ (cur_chg && point_in_poly(px + 0.75 - x, py + 0.25 - y, bolt, 6))) + ((px + 0.25 <= bx1) ^ (cur_chg && point_in_poly(px + 0.25 - x, py + 0.75 - y, bolt, 6))) + ((px + 0.75 <= bx1) ^ (cur_chg && point_in_poly(px + 0.75 - x, py + 0.75 - y, bolt, 6)));
	blend_pixel(px, py, c, hits * 64);
}

static void
draw_char(int x, int y, int ch, uint32_t c)
{
	int col, row;
	uint8_t a;

	ch = (ch >= 32 && ch <= 126) ? ch - 32 : 0;
	for (row = 0; row < FONT_HEIGHT; row++) {
		for (col = 0; col < FONT_WIDTH; col++) {
			a = font_data[ch][row * FONT_WIDTH + col];
			blend_pixel(x + col, y + row, c, a);
		}
	}
}

static void
draw_clock(int x, int y, uint32_t c)
{
	draw_arc(x + 10.0, y + 10.0, 6.8, 0, c, 1.6);
	draw_line(x + 10.0, y + 10.0, x + 10.0, y + 5.6, c, 1.6);
	draw_line(x + 10.0, y + 10.0, x + 13.2, y + 11.6, c, 1.6);
}

static void
draw_line(double x0, double y0, double x1, double y1, uint32_t c, double th)
{
	double d, dx, dy, l2, px, py, t;
	int x, y;

	dx = x1 - x0;
	dy = y1 - y0;
	l2 = dx * dx + dy * dy;
	for (y = (int)((y0 < y1 ? y0 : y1) - th); y <= (int)((y0 > y1 ? y0 : y1) + th + 1.0); y++) {
		for (x = (int)((x0 < x1 ? x0 : x1) - th); x <= (int)((x0 > x1 ? x0 : x1) + th + 1.0); x++) {
			t = l2 <= 0.0001 ? 0.0 : ((x + 0.5 - x0) * dx + (y + 0.5 - y0) * dy) / l2;
			t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
			px = x0 + t * dx;
			py = y0 + t * dy;
			d = hypot(x + 0.5 - px, y + 0.5 - py);
			blend_pixel(x, y, c, (int)((1.0 - (d - (th / 2.0 - 0.5))) * 255.0));
		}
	}
}

static int
draw_string(int x, int y, const char *s, uint32_t c)
{
	int ox;

	for (ox = x; *s; s++, x += FONT_WIDTH)
		draw_char(x, y, (unsigned char)*s, c);
	return x - ox;
}

static void
draw_volume(int x, int y, uint32_t c)
{
	int hits, i, lvl, px, py;

	for (py = y + 3; py <= y + 17; py++) {
		for (px = x + 1; px <= x + 8; px++) {
			hits = point_in_poly(px + 0.25 - x, py + 0.25 - y, spk, 6) + point_in_poly(px + 0.75 - x, py + 0.25 - y, spk, 6) + point_in_poly(px + 0.25 - x, py + 0.75 - y, spk, 6) + point_in_poly(px + 0.75 - x, py + 0.75 - y, spk, 6);
			blend_pixel(px, py, c, hits * 64);
		}
	}
	if (cur_mute) {
		draw_line(x + 10.4, y + 7.2, x + 15.6, y + 12.8, c, 1.6);
		draw_line(x + 15.6, y + 7.2, x + 10.4, y + 12.8, c, 1.6);
	} else {
		lvl = cur_vol <= 0 ? 0 : (cur_vol >= 100 ? 4 : (cur_vol * 4 + 50) / 100);
		for (i = 0; i < 4; i++)
			draw_arc(x + 6.8, y + 10.0, 2.8 + i * 2.8, 1, c, i < lvl ? 1.7 : 0.6);
	}
}

static void
ls_cfg(void *d, struct zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(s, serial);
	configured = 1;
	update_input_region();
	if (visible || !opt_autohide)
		update_all();
	draw();
}

static void
ls_closed(void *d, struct zwlr_layer_surface_v1 *s)
{
	running = 0;
}

static void
noop(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, wl_fixed_t v)
{
}

static int
point_in_poly(double px, double py, const double pts[][2], int n)
{
	int c, i, j;

	for (c = 0, i = 0, j = n - 1; i < n; j = i, i++) {
		if (((pts[i][1] > py) != (pts[j][1] > py)) && (px < (pts[j][0] - pts[i][0]) * (py - pts[i][1]) / (pts[j][1] - pts[i][1]) + pts[i][0]))
			c = !c;
	}
	return c;
}

static void
ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{
}

static void
ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
	set_visibility(1);
}

static void
ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf)
{
	if (opt_autohide)
		set_visibility(0);
}

static void
ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy)
{
}

static int
read_sysfs(const char *base, const char *entry)
{
	char b[32], p[128];
	int fd, n;

	snprintf(p, sizeof(p), "%s/%s", base, entry);
	if ((fd = open(p, O_RDONLY)) < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	b[n] = '\0';
	return b[0] == 'C' ? 'C' : atoi(b);
}

static void
reg_glob(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v)
{
	if (!strcmp(i, wl_compositor_interface.name))
		comp = wl_registry_bind(r, n, &wl_compositor_interface, 1);
	else if (!strcmp(i, wl_shm_interface.name))
		shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
	else if (!strcmp(i, zwlr_layer_shell_v1_interface.name))
		shell = wl_registry_bind(r, n, &zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(i, wl_seat_interface.name))
		wl_seat_add_listener(wl_registry_bind(r, n, &wl_seat_interface, 1), &seat_l, NULL);
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
set_visibility(int v)
{
	if (visible == v)
		return;
	visible = v;
	arm_timer(v);
	update_input_region();
	if (v)
		update_all();
	draw();
}

static void
setup(void)
{
	struct sockaddr_nl sa;
	struct wl_shm_pool *pool;
	char name[24];
	size_t sz;
	int fd;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	setup_mixer();
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = 1;
	if ((netlink_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT)) >= 0 && bind(netlink_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(netlink_fd);
		netlink_fd = -1;
	}

	if (!(dpy = wl_display_connect(NULL)))
		die("cannot connect to wayland display");
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !shell)
		die("missing wayland globals");

	sz = (size_t)bar_width * 4 * bar_height;
	snprintf(name, sizeof(name), "/bar%d", (int)getpid());
	if ((fd = shm_open(name, O_CREAT | O_RDWR, 0600)) < 0)
		die("shm_open:");
	shm_unlink(name);
	if (ftruncate(fd, sz) < 0 || (pixel_data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
		die("mmap:");
	pool = wl_shm_create_pool(shm, fd, sz);
	buf = wl_shm_pool_create_buffer(pool, 0, bar_width, bar_height, bar_width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	surf = wl_compositor_create_surface(comp);
	ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
	zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
	zwlr_layer_surface_v1_set_size(ls, bar_width, bar_height);
	zwlr_layer_surface_v1_set_anchor(ls, anchors[selected_corner & 3]);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, 0);
	wl_surface_commit(surf);
	wl_display_roundtrip(dpy);
}

static int
setup_alsa_poll(struct pollfd *pfds)
{
	int count;

	if (!mixer)
		return 0;
	count = snd_mixer_poll_descriptors_count(mixer);
	if (count <= 0 || count > 5)
		return 0;
	snd_mixer_poll_descriptors(mixer, pfds, count);
	return count;
}

static void
setup_mixer(void)
{
	snd_mixer_elem_t *elem;

	if (mixer)
		snd_mixer_close(mixer);
	mixer = NULL;
	mixer_elem = NULL;
	if (snd_mixer_open(&mixer, 0) < 0)
		return;
	if (snd_mixer_attach(mixer, alsa_device) < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 || snd_mixer_load(mixer) < 0) {
		snd_mixer_close(mixer);
		mixer = NULL;
		return;
	}
	for (elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem)) {
		if (!strcmp(snd_mixer_selem_get_name(elem), alsa_mixer)) {
			mixer_elem = elem;
			break;
		}
	}
}

static void
sig_handler(int sig)
{
	running = 0;
}

static void
update_all(void)
{
	update_sysfs();
	update_volume();
	update_clock();
}

static int
update_clock(void)
{
	char b[32];
	time_t t;

	t = time(NULL);
	strftime(b, sizeof(b), time_format, localtime(&t));
	if (!strcmp(b, cur_time))
		return 0;
	strcpy(cur_time, b);
	return 1;
}

static void
update_input_region(void)
{
	struct wl_region *r;
	int h, w, x, y;

	if (!(r = wl_compositor_create_region(comp)))
		return;
	x = y = 0;
	w = bar_width;
	h = bar_height;
	if (!visible && opt_autohide) {
		w = h = hotspot_size;
		x = (selected_corner & 1) ? bar_width - hotspot_size : 0;
		y = (selected_corner & 2) ? bar_height - hotspot_size : 0;
	}
	wl_region_add(r, x, y, w, h);
	wl_surface_set_input_region(surf, r);
	wl_region_destroy(r);
}

static int
update_sysfs(void)
{
	int b, c, cur, max, mod, v;

	cur = read_sysfs(backlight_path, "brightness");
	max = read_sysfs(backlight_path, "max_brightness");
	v = (cur < 0 || max <= 0) ? 0 : (cur * 100 + max / 2) / max;
	if ((b = read_sysfs(battery_path, "capacity")) < 0)
		b = 0;
	c = read_sysfs(battery_path, "status") == 'C';
	mod = (v != cur_bright || b != cur_bat || c != cur_chg);
	cur_bright = v;
	cur_bat = b;
	cur_chg = c;
	return mod;
}

static int
update_volume(void)
{
	long max, min, vol;
	int muted, sw, v;

	if (!mixer_elem) {
		setup_mixer();
		if (!mixer_elem)
			return 0;
	}
	sw = 1;
	vol = min = max = 0;
	if (snd_mixer_selem_get_playback_volume_range(mixer_elem, &min, &max) < 0 || snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_MONO, &vol) < 0) {
		setup_mixer();
		return 0;
	}
	snd_mixer_selem_get_playback_switch(mixer_elem, SND_MIXER_SCHN_MONO, &sw);
	muted = !sw;
	v = max > min ? (vol - min) * 100 / (max - min) : 0;
	if (v == cur_vol && muted == cur_mute)
		return 0;
	cur_vol = v;
	cur_mute = muted;
	return 1;
}

static void
usage(void)
{
	die("usage: bar [-c corner] [-n] [-v]");
}

int
main(int argc, char **argv)
{
	struct pollfd pfds[8];
	uint64_t exp;
	char nlk[512];
	int alsa_count, dirty, nfds, opt;

	selected_corner = corner;
	opt_autohide = autohide;
	if (!opt_autohide)
		visible = 1;
	alsa_count = 0;
	while ((opt = getopt(argc, argv, "c:nhv")) >= 0) {
		switch (opt) {
		case 'c':
			selected_corner = (optarg[0] >= '0' && optarg[0] <= '3') ? optarg[0] - '0' : ((optarg[0] == 'b') << 1) | (optarg[1] == 'r');
			break;
		case 'n':
			opt_autohide = 0;
			visible = 1;
			break;
		case 'v':
			puts("bar-" VERSION);
			return 0;
		default:
			usage();
		}
	}

	setup();
	if (timer_fd >= 0 && visible)
		arm_timer(1);
	update_all();

	memset(pfds, 0, sizeof(pfds));
	pfds[0].fd = wl_display_get_fd(dpy);
	pfds[0].events = POLLIN;
	pfds[1].fd = timer_fd;
	pfds[1].events = POLLIN;
	pfds[2].fd = netlink_fd;
	pfds[2].events = POLLIN;

	while (running) {
		dirty = 0;
		if (!mixer_elem)
			dirty |= update_volume();
		alsa_count = setup_alsa_poll(&pfds[3]);
		nfds = 3 + alsa_count;

		while (wl_display_prepare_read(dpy) < 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		if (poll(pfds, nfds, -1) < 0) {
			wl_display_cancel_read(dpy);
			running &= (errno == EINTR);
			continue;
		}

		if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP))
			wl_display_read_events(dpy);
		else
			wl_display_cancel_read(dpy);
		wl_display_dispatch_pending(dpy);

		if (timer_fd >= 0 && (pfds[1].revents & POLLIN) && read(timer_fd, &exp, sizeof(exp)) > 0)
			dirty |= update_clock();
		if (netlink_fd >= 0 && (pfds[2].revents & POLLIN)) {
			while (recv(netlink_fd, nlk, sizeof(nlk), 0) > 0) {}
			dirty |= update_sysfs();
		}
		if (alsa_count > 0)
			dirty |= check_alsa(&pfds[3], alsa_count);
		if (dirty && visible)
			draw();
	}

	cleanup();
	return 0;
}
