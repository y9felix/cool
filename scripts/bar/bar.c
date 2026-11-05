#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
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
static void create_surface(void);
static void destroy_surface(void *data, struct zwlr_layer_surface_v1 *surface);
static void die(const char *fmt, ...);
static void draw(void);
static void draw_arc(double cx, double cy, double r, int arc, uint32_t c, double th);
static void draw_backlight(int x, int y, uint32_t c);
static void draw_battery(int x, int y, uint32_t c);
static void draw_line(double x0, double y0, double x1, double y1, uint32_t c, double th);
static void draw_text(int *x, int y, int isz, uint32_t c, const char *fmt, ...);
static void draw_volume(int x, int y, uint32_t c);
static void ls_cfg(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t w, uint32_t h);
static int point_in_poly(double px, double py, const double pts[][2]);
static void ptr_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
static void ptr_btn(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void ptr_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy);
static void ptr_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
static void ptr_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy);
static int pulse_connect(void);
static void pulse_disconnect(void);
static int pulse_req(uint32_t cmd, const uint8_t *b, size_t blen, uint8_t *resp, size_t rlen);
static int read_sysfs(const char *base, const char *entry);
static int recv_buf(void *buf, size_t len);
static void reg_glob(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void reg_remove(void *data, struct wl_registry *registry, uint32_t name);
static void seat_caps(void *data, struct wl_seat *seat, uint32_t capabilities);
static void set_visibility(int visible_new);
static void sig_handler(int sig);
static uint32_t unpack_be32(const uint8_t *p);
static int update_clock(void);
static void update_input_region(void);
static int update_sysfs(void);
static int update_volume(void);

#include "config.h"

const struct wl_interface xdg_popup_interface;

static const double bolt[][2] = {
	{ 13.41,  5.56 }, {  6.90, 10.44 }, { 10.26, 10.44 },
	{  9.21, 14.44 }, { 15.30,  9.33 }, { 11.94,  9.33 }
};

static const double spk[][2] = {
	{ 1.2,  7.6 }, { 4.2,  7.6 }, { 7.2,  3.6 },
	{ 7.2, 16.4 }, { 4.2, 12.4 }, { 1.2, 12.4 }
};

static const double subpix[4][2] = {
	{ 0.25, 0.25 }, { 0.75, 0.25 },
	{ 0.25, 0.75 }, { 0.75, 0.75 }
};

static const uint32_t anchors[] = {
	ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
};

static const struct wl_pointer_listener ptr_l = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_btn, .axis = ptr_axis
};
static const struct wl_seat_listener seat_l = { .capabilities = seat_caps };
static const struct zwlr_layer_surface_v1_listener ls_l = { .configure = ls_cfg, .closed = destroy_surface };
static const struct wl_registry_listener reg_l = { .global = reg_glob, .global_remove = reg_remove };

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *shell;
static struct zwlr_layer_surface_v1 *ls;
static struct wl_surface *surf;
static struct wl_buffer *buf;
static struct wl_pointer *ptr;
static void *pixel_data;
static uint32_t output_names[16], pulse_tag;
static int output_count, timer_fd = -1, netlink_fd = -1, pulse_fd = -1, selected_corner, opt_autohide, visible, configured;
static int cur_bright = -1, cur_vol = -1, cur_bat = -1, cur_mute = -1, cur_chg = -1;
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
	its.it_interval.tv_sec = enable;
	if (enable && clock_gettime(CLOCK_REALTIME, &ts) == 0)
		its.it_value.tv_nsec = 1000000000L - ts.tv_nsec;

	timerfd_settime(timer_fd, 0, &its, NULL);
}

static void
blend_pixel(int x, int y, uint32_t c, int a)
{
	uint32_t *p, bg, fa, ia;
	int s;

	if ((unsigned)x >= (unsigned)bar_width || (unsigned)y >= (unsigned)bar_height || a <= 0)
		return;

	p = (uint32_t *)pixel_data + y * bar_width + x;
	fa = (((c >> 24) & 0xff) * fmin(a, 255)) / 255;
	if (fa >= 255) {
		*p = c;
	} else if (fa > 0) {
		bg = *p;
		ia = 255 - fa;
		*p = 0;
		for (s = 0; s < 32; s += 8)
			*p |= (uint32_t)((((c >> s) & 0xff) * fa + ((bg >> s) & 0xff) * ia) / 255) << s;
	}
}

static double
box_dist(double px, double py, double cx, double cy, double rx, double ry, double r)
{
	double qx, qy;

	qx = fabs(px - cx) - rx;
	qy = fabs(py - cy) - ry;
	return hypot(fmax(qx, 0.0), fmax(qy, 0.0)) + fmin(fmax(qx, qy), 0.0) - r;
}

static void
create_surface(void)
{
	if (ls || !output_count || !comp || !shell)
		return;

	surf = wl_compositor_create_surface(comp);
	ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
	zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
	zwlr_layer_surface_v1_set_size(ls, bar_width, bar_height);
	zwlr_layer_surface_v1_set_anchor(ls, anchors[selected_corner]);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, 0);
	wl_surface_commit(surf);
}

static void
destroy_surface(void *data, struct zwlr_layer_surface_v1 *surface)
{
	configured = 0;
	if (ls)
		zwlr_layer_surface_v1_destroy(ls);
	if (surf)
		wl_surface_destroy(surf);
	ls = NULL;
	surf = NULL;
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
	uint32_t c, *p;
	int i, icon_y, text_y, x;

	if (!configured || !pixel_data || !surf || !ls)
		return;

	if (opt_autohide && !visible) {
		memset(pixel_data, 0, (size_t)bar_width * 4 * bar_height);
	} else {
		p = pixel_data;
		for (i = 0; i < bar_width * bar_height; i++)
			p[i] = color_bg;

		icon_y = fmax((bar_height - icon_size) / 2, 0);
		text_y = (bar_height - FONT_HEIGHT) / 2;
		x = padding_x;

		draw_backlight(x, icon_y, color_fg);
		draw_text(&x, text_y, icon_size, color_fg, "%d%%", cur_bright);

		c = cur_mute ? color_disabled : color_fg;
		draw_volume(x, icon_y, c);
		draw_text(&x, text_y, icon_size, c, "%d%%", cur_vol);

		draw_battery(x, icon_y, color_fg);
		draw_text(&x, text_y, (int)(1.25 * icon_size + 0.5), color_fg, "%d%%", cur_bat);

		draw_arc(x + 10.0, icon_y + 10.0, 6.8, 0, color_fg, 1.6);
		draw_line(x + 10.0, icon_y + 10.0, x + 10.0, icon_y + 5.6, color_fg, 1.6);
		draw_line(x + 10.0, icon_y + 10.0, x + 13.2, icon_y + 11.6, color_fg, 1.6);
		draw_text(&x, text_y, icon_size, color_fg, "%s", cur_time);
	}

	wl_surface_damage(surf, 0, 0, bar_width, bar_height);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
}

static void
draw_arc(double cx, double cy, double r, int arc, uint32_t c, double th)
{
	double d, dx, dy, hw;
	int x, y;

	hw = th * 0.5 + 0.5;

	for (y = cy - r - th; y <= cy + r + th + 1.0; y++) {
		for (x = cx - r - th; x <= cx + r + th + 1.0; x++) {
			dx = x + 0.5 - cx;
			dy = y + 0.5 - cy;
			if (!arc || (dx > 0.0 && fabs(dy) <= 1.1155 * dx)) {
				d = fabs(hypot(dx, dy) - r);
				blend_pixel(x, y, c, (hw - d) * 255.0);
			}
		}
	}
}

static void
draw_backlight(int x, int y, uint32_t c)
{
	double ax, d, dx, dy, w;
	int lvl, px, py;

	draw_arc(x + 10.0, y + 10.0, 6.8, 0, c, 1.7);

	if (cur_bright <= 0)
		lvl = 0;
	else if (cur_bright >= 100)
		lvl = 6;
	else
		lvl = (cur_bright * 6 + 50) / 100;

	if (lvl <= 0)
		return;

	ax = 5.95 * ((double)lvl / 3.0 - 1.0);
	for (py = y + 3; py <= y + 17; py++) {
		dy = py + 0.5 - (y + 10.0);
		if (fabs(dy) < 5.95) {
			w = sqrt(1.0 - (dy / 5.95) * (dy / 5.95));
			for (px = x + 3; px <= x + 17; px++) {
				dx = px + 0.5 - (x + 10.0);
				if (dx >= -5.95 * w && dx <= ax * w) {
					d = hypot(dx, dy);
					blend_pixel(px, py, c, (d > 5.45 ? 6.45 - d : 1.0) * 255.0);
				}
			}
		}
	}
}

static void
draw_battery(int x, int y, uint32_t c)
{
	double bx1, d;
	int hits, in_bolt, in_fill, px, py, s;

	bx1 = x + 2.0 + 0.182 * fmin(fmax(cur_bat, 0), 100);

	for (py = y + 2; py <= y + 18; py++) {
		for (px = x; px <= x + 25; px++) {
			d = box_dist(px + 0.5, py + 0.5, x + 11.1, y + 10.0, 8.7, 4.6, 1.6);
			blend_pixel(px, py, c, (1.3 - fabs(d)) * 255.0);

			d = box_dist(px + 0.5, py + 0.5, x + 23.5, y + 10.0, 0.5, 1.4, 0.6);
			blend_pixel(px, py, c, (0.5 - d) * 255.0);

			if (px >= x + 2 && px <= x + 20 && py >= y + 5 && py <= y + 14 && (bx1 > x + 2.0 || cur_chg)) {
				hits = 0;
				for (s = 0; s < 4; s++) {
					in_fill = (px + subpix[s][0] <= bx1);
					if (cur_chg && point_in_poly(px + subpix[s][0] - x, py + subpix[s][1] - y, bolt))
						in_bolt = 1;
					else
						in_bolt = 0;
					if (in_fill != in_bolt)
						hits++;
				}
				blend_pixel(px, py, c, hits * 64);
			}
		}
	}
}

static void
draw_line(double x0, double y0, double x1, double y1, uint32_t c, double th)
{
	double dx, dy, hw, l2, t;
	int x, y;

	dx = x1 - x0;
	dy = y1 - y0;
	hw = th * 0.5 + 0.5;
	l2 = dx * dx + dy * dy;

	for (y = fmin(y0, y1) - th; y <= fmax(y0, y1) + th + 1.0; y++) {
		for (x = fmin(x0, x1) - th; x <= fmax(x0, x1) + th + 1.0; x++) {
			if (l2 <= 0.0001)
				t = 0.0;
			else
				t = fmin(fmax(((x + 0.5 - x0) * dx + (y + 0.5 - y0) * dy) / l2, 0.0), 1.0);
			blend_pixel(x, y, c, (hw - hypot(x + 0.5 - (x0 + t * dx), y + 0.5 - (y0 + t * dy))) * 255.0);
		}
	}
}

static void
draw_text(int *x, int y, int isz, uint32_t c, const char *fmt, ...)
{
	va_list ap;
	char s[32], *p;
	int ch, i;

	*x += isz + icon_spacing;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);

	for (p = s; *p != '\0'; p++) {
		ch = (unsigned char)*p;
		if (ch >= 32 && ch <= 126)
			ch -= 32;
		else
			ch = 0;
		for (i = 0; i < FONT_WIDTH * FONT_HEIGHT; i++)
			blend_pixel(*x + i % FONT_WIDTH, y + i / FONT_WIDTH, c, font_data[ch][i]);
		*x += FONT_WIDTH;
	}
	*x += spacing;
}

static void
draw_volume(int x, int y, uint32_t c)
{
	double th;
	int hits, i, lvl, px, py, s;

	for (py = y + 3; py <= y + 17; py++) {
		for (px = x + 1; px <= x + 8; px++) {
			hits = 0;
			for (s = 0; s < 4; s++) {
				if (point_in_poly(px + subpix[s][0] - x, py + subpix[s][1] - y, spk))
					hits++;
			}
			blend_pixel(px, py, c, hits * 64);
		}
	}

	if (cur_mute) {
		draw_line(x + 10.4, y + 7.2, x + 15.6, y + 12.8, c, 1.6);
		draw_line(x + 15.6, y + 7.2, x + 10.4, y + 12.8, c, 1.6);
	} else {
		if (cur_vol <= 0)
			lvl = 0;
		else if (cur_vol >= 100)
			lvl = 4;
		else
			lvl = (cur_vol * 4 + 50) / 100;

		for (i = 0; i < 4; i++) {
			th = (i < lvl) ? 1.7 : 0.6;
			draw_arc(x + 6.8, y + 10.0, 2.8 + i * 2.8, 1, c, th);
		}
	}
}

static void
ls_cfg(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	configured = 1;
	update_input_region();
	if (visible) {
		update_sysfs();
		update_volume();
		update_clock();
	}
	draw();
}

static int
point_in_poly(double px, double py, const double pts[][2])
{
	int c, i, j;

	c = 0;
	j = 5;
	for (i = 0; i < 6; i++) {
		if ((pts[i][1] > py) != (pts[j][1] > py) &&
		    px < (pts[j][0] - pts[i][0]) * (py - pts[i][1]) / (pts[j][1] - pts[i][1]) + pts[i][0])
			c = !c;
		j = i;
	}
	return c;
}

static void
ptr_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
ptr_btn(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
}

static void
ptr_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	set_visibility(1);
}

static void
ptr_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
	set_visibility(0);
}

static void
ptr_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
}

static int
pulse_connect(void)
{
	struct sockaddr_un sa;
	struct timeval tv;
	struct passwd *pw;
	const char *home, *rundir;
	uint8_t body[266], resp[1024];
	char path[256];
	int cfd;

	pulse_disconnect();

	tv.tv_sec = 0;
	tv.tv_usec = 250000;
	memset(body, 0, sizeof(body));
	body[0] = 'L';
	body[4] = 35;
	body[5] = 'x';
	body[8] = 1;

	home = getenv("HOME");
	if (!home) {
		pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}
	if (!home)
		return -1;

	snprintf(path, sizeof(path), "%s/.config/pulse/cookie", home);

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	rundir = getenv("XDG_RUNTIME_DIR");
	if (rundir)
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/pulse/native", rundir);
	else
		snprintf(sa.sun_path, sizeof(sa.sun_path), "/run/user/%u/pulse/native", (unsigned)getuid());

	pulse_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (pulse_fd < 0)
		return -1;

	setsockopt(pulse_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (connect(pulse_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		pulse_disconnect();
		return -1;
	}

	cfd = open(path, O_RDONLY);
	if (cfd < 0) {
		pulse_disconnect();
		return -1;
	}

	if (read(cfd, body + 10, 256) != 256) {
		close(cfd);
		pulse_disconnect();
		return -1;
	}
	close(cfd);

	if (pulse_req(8, body, sizeof(body), resp, sizeof(resp)) < 0 ||
	    pulse_req(9, (const uint8_t *)"Ptapplication.name\0L\0\0\0\4x\0\0\0\4bar\0N", 34, resp, sizeof(resp)) < 0 ||
	    pulse_req(35, (const uint8_t *)"L\0\0\x02\xff", 5, resp, sizeof(resp)) < 0) {
		pulse_disconnect();
		return -1;
	}

	return 0;
}

static void
pulse_disconnect(void)
{
	if (pulse_fd >= 0)
		close(pulse_fd);
	pulse_fd = -1;
}

static int
pulse_req(uint32_t cmd, const uint8_t *b, size_t blen, uint8_t *resp, size_t rlen)
{
	uint8_t buf[512];
	size_t len;
	uint32_t tag;

	tag = pulse_tag;
	pulse_tag++;

	memset(buf, 0, 30);
	buf[2] = (blen + 10) >> 8;
	buf[3] = (blen + 10) & 0xff;
	memset(buf + 4, 0xff, 4);
	buf[20] = 'L';
	buf[21] = cmd >> 24;
	buf[22] = cmd >> 16;
	buf[23] = cmd >> 8;
	buf[24] = cmd & 0xff;
	buf[25] = 'L';
	buf[26] = tag >> 24;
	buf[27] = tag >> 16;
	buf[28] = tag >> 8;
	buf[29] = tag & 0xff;

	if (blen > 0)
		memcpy(buf + 30, b, blen);

	if (send(pulse_fd, buf, 30 + blen, MSG_NOSIGNAL) != (ssize_t)(30 + blen))
		return -1;

	while (1) {
		if (recv_buf(resp, 20) < 0)
			return -1;
		len = unpack_be32(resp);
		if (recv_buf(resp, fmin(len, rlen)) < 0)
			return -1;
		if (len >= 10 && resp[0] == 'L' && resp[5] == 'L' && unpack_be32(resp + 6) == tag) {
			if (unpack_be32(resp + 1) == 2)
				return (int)fmin(len, rlen);
			return -1;
		}
	}
}

static int
read_sysfs(const char *base, const char *entry)
{
	char b[32], p[128];
	int fd, n;

	snprintf(p, sizeof(p), "%s/%s", base, entry);
	fd = open(p, O_RDONLY);
	if (fd < 0)
		return -1;

	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -1;

	b[n] = '\0';
	if (b[0] == 'C')
		return 'C';
	return atoi(b);
}

static int
recv_buf(void *buf, size_t len)
{
	size_t off;
	ssize_t n;

	for (off = 0; off < len; off += n) {
		n = recv(pulse_fd, (char *)buf + off, len - off, 0);
		if (n <= 0)
			return -1;
	}
	return 0;
}

static void
reg_glob(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name)) {
		comp = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		wl_seat_add_listener(wl_registry_bind(registry, name, &wl_seat_interface, 1), &seat_l, NULL);
	} else if (!strcmp(interface, wl_output_interface.name)) {
		if (output_count < 16) {
			output_names[output_count] = name;
			output_count++;
			create_surface();
		}
	}
}

static void
reg_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	int i;

	for (i = 0; i < output_count; i++) {
		if (output_names[i] == name) {
			output_count--;
			output_names[i] = output_names[output_count];
			break;
		}
	}
	if (!output_count && ls)
		destroy_surface(NULL, NULL);
}

static void
seat_caps(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
		ptr = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ptr, &ptr_l, NULL);
	} else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && ptr) {
		wl_pointer_destroy(ptr);
		ptr = NULL;
	}
}

static void
set_visibility(int visible_new)
{
	int v;

	if (opt_autohide)
		v = visible_new;
	else
		v = 1;

	if (visible != v) {
		visible = v;
		arm_timer(v);
		update_input_region();
		if (v) {
			update_sysfs();
			update_volume();
			update_clock();
		}
		draw();
	}
}

static void
sig_handler(int sig)
{
	running = 0;
}

static uint32_t
unpack_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) |
	       ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] <<  8) |
	       (uint32_t)p[3];
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
	int hh, hw, hx, hy;

	if (!surf)
		return;

	r = wl_compositor_create_region(comp);
	if (!r)
		return;

	if (opt_autohide && !visible) {
		if (selected_corner == CORNER_TOP_RIGHT || selected_corner == CORNER_BOTTOM_RIGHT)
			hx = bar_width - hotspot_size;
		else
			hx = 0;

		if (selected_corner == CORNER_BOTTOM_LEFT || selected_corner == CORNER_BOTTOM_RIGHT)
			hy = bar_height - hotspot_size;
		else
			hy = 0;

		hw = hotspot_size;
		hh = hotspot_size;
	} else {
		hx = 0;
		hy = 0;
		hw = bar_width;
		hh = bar_height;
	}

	wl_region_add(r, hx, hy, hw, hh);
	wl_surface_set_input_region(surf, r);
	wl_region_destroy(r);
}

static int
update_sysfs(void)
{
	int b, c, cur, max, v;

	b = read_sysfs(battery_path, "capacity");
	if (b < 0)
		b = 0;
	if (read_sysfs(battery_path, "status") == 'C')
		c = 1;
	else
		c = 0;

	cur = read_sysfs(backlight_path, "brightness");
	max = read_sysfs(backlight_path, "max_brightness");
	if (cur < 0 || max <= 0)
		v = 0;
	else
		v = (cur * 100 + max / 2) / max;

	if (v == cur_bright && b == cur_bat && c == cur_chg)
		return 0;

	cur_bright = v;
	cur_bat = b;
	cur_chg = c;
	return 1;
}

static int
update_volume(void)
{
	uint8_t resp[4096];
	int chans, len, mute, v, v_idx;

	if (pulse_fd < 0 && pulse_connect() < 0)
		return 0;

	len = pulse_req(21, (const uint8_t *)"L\xff\xff\xff\xfft@DEFAULT_SINK@", 21, resp, sizeof(resp));
	if (len < 0) {
		pulse_disconnect();
		return 0;
	}

	mute = 0;
	v = -1;
	for (v_idx = 0; v_idx < len - 10; v_idx++) {
		if (resp[v_idx] == 'v') {
			chans = resp[v_idx + 1];
			v = (unpack_be32(resp + v_idx + 2) * 100 + 32768) / 65536;
			if (v > 100)
				v = 100;
			if (v_idx + 2 + 4 * chans < len && resp[v_idx + 2 + 4 * chans] == '1')
				mute = 1;
			else
				mute = 0;
			break;
		}
	}

	if (v < 0 || (v == cur_vol && mute == cur_mute))
		return 0;

	cur_vol = v;
	cur_mute = mute;
	return 1;
}

int
main(int argc, char **argv)
{
	struct sockaddr_nl sa;
	struct wl_shm_pool *pool;
	struct pollfd pfds[4];
	uint64_t exp;
	size_t sz;
	char nlk[512];
	int dirty, fd, i, opt;

	selected_corner = corner;
	opt_autohide = autohide;
	visible = !opt_autohide;

	while ((opt = getopt(argc, argv, "c:nhv")) >= 0) {
		switch (opt) {
		case 'c':
			if (optarg[0] >= '0' && optarg[0] <= '3') {
				selected_corner = optarg[0] - '0';
			} else if (!strcmp(optarg, "tl")) {
				selected_corner = CORNER_TOP_LEFT;
			} else if (!strcmp(optarg, "tr")) {
				selected_corner = CORNER_TOP_RIGHT;
			} else if (!strcmp(optarg, "bl")) {
				selected_corner = CORNER_BOTTOM_LEFT;
			} else if (!strcmp(optarg, "br")) {
				selected_corner = CORNER_BOTTOM_RIGHT;
			} else {
				die("invalid corner: %s", optarg);
			}
			break;
		case 'n':
			opt_autohide = 0;
			visible = 1;
			break;
		case 'v':
			puts("bar-" VERSION);
			return 0;
		default:
			die("usage: bar [-c corner] [-n] [-v]");
		}
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	for (i = 0; i < 4; i++) {
		pfds[i].fd = -1;
		pfds[i].events = POLLIN;
		pfds[i].revents = 0;
	}

	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	pfds[1].fd = timer_fd;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = 1;

	netlink_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	pfds[2].fd = netlink_fd;
	if (netlink_fd >= 0) {
		if (bind(netlink_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(netlink_fd);
			netlink_fd = -1;
			pfds[2].fd = -1;
		}
	}

	dpy = wl_display_connect(NULL);
	if (!dpy)
		die("cannot connect to wayland display");

	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !shell)
		die("missing wayland globals");

	sz = (size_t)bar_width * 4 * bar_height;
	fd = memfd_create("bar", MFD_CLOEXEC);
	if (fd < 0)
		die("memfd_create:");

	if (ftruncate(fd, sz) < 0)
		die("ftruncate:");
	pixel_data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixel_data == MAP_FAILED)
		die("mmap:");

	pool = wl_shm_create_pool(shm, fd, sz);
	buf = wl_shm_pool_create_buffer(pool, 0, bar_width, bar_height, bar_width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	create_surface();
	wl_display_roundtrip(dpy);

	arm_timer(visible);
	update_sysfs();
	update_volume();
	update_clock();
	pfds[0].fd = wl_display_get_fd(dpy);

	while (running) {
		create_surface();
		if (pulse_fd < 0)
			dirty = update_volume();
		else
			dirty = 0;
		pfds[3].fd = pulse_fd;

		while (wl_display_prepare_read(dpy) < 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		if (poll(pfds, 4, -1) < 0) {
			wl_display_cancel_read(dpy);
			if (errno != EINTR)
				running = 0;
			continue;
		}

		if (!(pfds[0].revents & (POLLIN | POLLERR | POLLHUP)))
			wl_display_cancel_read(dpy);
		else if (wl_display_read_events(dpy) < 0)
			break;

		if (wl_display_dispatch_pending(dpy) < 0)
			break;

		if ((pfds[1].revents & POLLIN) && read(timer_fd, &exp, sizeof(exp)) > 0) {
			if (update_clock())
				dirty = 1;
		}

		if (pfds[2].revents & POLLIN) {
			while (recv(netlink_fd, nlk, sizeof(nlk), 0) > 0)
				;
			if (update_sysfs())
				dirty = 1;
		}

		if (pfds[3].revents & (POLLERR | POLLHUP)) {
			pulse_disconnect();
		} else if (pfds[3].revents & POLLIN) {
			if (update_volume())
				dirty = 1;
		}

		if (dirty && visible)
			draw();
	}

	destroy_surface(NULL, NULL);
	pulse_disconnect();
	wl_display_disconnect(dpy);
	return 0;
}
