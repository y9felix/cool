#define _GNU_SOURCE

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
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "font.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef VERSION
#define VERSION "1.0"
#endif

enum {
	OSD_VOLUME,
	OSD_BRIGHTNESS
};

static void arm_timer(int seconds);
static void blend_pixel(int x, int y, uint32_t c, int a);
static void cleanup(void);
static void create_surface(void);
static void destroy_surface(void);
static void die(const char *fmt, ...);
static void draw(void);
static void draw_arc(double cx, double cy, double r, int arc, uint32_t c, double th);
static void draw_backlight(int x, int y, uint32_t c);
static void draw_char(int x, int y, int ch, uint32_t c);
static void draw_line(double x0, double y0, double x1, double y1, uint32_t c, double th);
static int draw_string(int x, int y, const char *s, uint32_t c);
static void draw_volume(int x, int y, uint32_t c);
static void fill(uint32_t *p, int w, int h, int x, int y, int rw, int rh, uint32_t col);
static void ls_cfg(void *d, struct zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h);
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *s);
static int point_in_poly(double px, double py, const double pts[][2], int n);
static int pulse_connect(void);
static void pulse_disconnect(void);
static int pulse_recv_packet(int fd, uint8_t *buf, size_t max_len);
static int pulse_recv_reply(int fd, uint32_t req_tag, uint8_t *buf, size_t max_len);
static int pulse_send_cmd(int fd, uint32_t cmd, uint32_t tag, const uint8_t *body, size_t body_len);
static int read_sysfs(const char *base, const char *entry);
static void reg_glob(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v);
static void reg_remove(void *d, struct wl_registry *r, uint32_t n);
static void set_visibility(int v);
static void setup(void);
static void sig_handler(int sig);
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
static const int margins[4][4] = {
	{ 1, 0, 0, 1 },
	{ 1, 1, 0, 0 },
	{ 0, 0, 1, 1 },
	{ 0, 1, 1, 0 }
};
static const double spk[6][2] = {
	{ 1.2,  7.6 }, { 4.2,  7.6 }, { 7.2,  3.6 },
	{ 7.2, 16.4 }, { 4.2, 12.4 }, { 1.2, 12.4 }
};

static const struct zwlr_layer_surface_v1_listener ls_l = {
	.configure = ls_cfg,
	.closed = ls_closed
};
static const struct wl_registry_listener reg_l = {
	.global = reg_glob,
	.global_remove = reg_remove
};

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *shell;
static struct wl_surface *surf;
static struct zwlr_layer_surface_v1 *ls;
static struct wl_buffer *buf;
static uint32_t *pixel_data;
static uint32_t output_names[16];
static int output_count;
static int timer_fd = -1;
static int netlink_fd = -1;
static int pulse_fd = -1;
static uint32_t pulse_tag;
static int cur_bright = -1;
static int cur_vol = -1;
static int cur_mute;
static int osd_type = OSD_VOLUME;
static int selected_corner;
static int visible;
static int configured;
static volatile sig_atomic_t running = 1;

static void
arm_timer(int seconds)
{
	struct itimerspec its;

	if (timer_fd < 0)
		return;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = seconds;
	timerfd_settime(timer_fd, 0, &its, NULL);
}

static void
blend_pixel(int x, int y, uint32_t c, int a)
{
	uint32_t bg, fa, ia, *p;

	if (x < 0 || x >= popup_width || y < 0 || y >= popup_height || a <= 0)
		return;
	p = pixel_data + y * popup_width + x;
	if ((fa = (((c >> 24) & 0xff) * (a > 255 ? 255 : a)) / 255) >= 255) {
		*p = c;
		return;
	}
	if (!fa)
		return;
	bg = *p;
	ia = 255 - fa;
	*p = ((fa + (((bg >> 24) & 0xff) * ia) / 255) << 24)
	   | (((((c >> 16) & 0xff) * fa + ((bg >> 16) & 0xff) * ia) / 255) << 16)
	   | (((((c >> 8) & 0xff) * fa + ((bg >> 8) & 0xff) * ia) / 255) << 8)
	   | (((c & 0xff) * fa + (bg & 0xff) * ia) / 255);
}

static void
cleanup(void)
{
	pulse_disconnect();
	if (timer_fd >= 0)
		close(timer_fd);
	if (netlink_fd >= 0)
		close(netlink_fd);
	if (buf)
		wl_buffer_destroy(buf);
	destroy_surface();
	if (reg)
		wl_registry_destroy(reg);
	if (shm)
		wl_shm_destroy(shm);
	if (shell)
		zwlr_layer_shell_v1_destroy(shell);
	if (comp)
		wl_compositor_destroy(comp);
	if (dpy) {
		wl_display_flush(dpy);
		wl_display_disconnect(dpy);
	}
}

static void
create_surface(void)
{
	if (ls || !comp || !shell)
		return;
	surf = wl_compositor_create_surface(comp);
	ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "osd");
	zwlr_layer_surface_v1_set_size(ls, popup_width, popup_height);
	zwlr_layer_surface_v1_set_anchor(ls, anchors[selected_corner]);
	zwlr_layer_surface_v1_set_margin(ls,
		margins[selected_corner][0] * edge_margin,
		margins[selected_corner][1] * edge_margin,
		margins[selected_corner][2] * edge_margin,
		margins[selected_corner][3] * edge_margin);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
	zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
	wl_surface_commit(surf);
}

static void
destroy_surface(void)
{
	configured = 0;
	visible = 0;
	if (ls) {
		zwlr_layer_surface_v1_destroy(ls);
		ls = NULL;
	}
	if (surf) {
		wl_surface_destroy(surf);
		surf = NULL;
	}
}

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void
draw(void)
{
	char s[32];
	uint32_t border_col, col;
	int active_sq, bg_h, bg_w, bx, by, i, pct, rem, sq_x, sx, total_sq, tx, ty;

	if (!configured || !visible || !surf || !ls)
		return;

	fill(pixel_data, popup_width, popup_height, 0, 0, popup_width, popup_height, 0);

	border_col = (osd_type == OSD_VOLUME && cur_mute) ? color_disabled : color_accent;
	bg_w = popup_width - border_width * 2;
	bg_h = popup_height - border_width * 2;
	fill(pixel_data, popup_width, popup_height, 0, 0, popup_width, popup_height, border_col);
	fill(pixel_data, popup_width, popup_height, border_width, border_width, bg_w, bg_h, color_bg);

	bx = border_width + padding_x;
	by = border_width + (bg_h - icon_size) / 2;
	if (osd_type == OSD_VOLUME)
		draw_volume(bx, by, cur_mute ? color_disabled : color_text);
	else
		draw_backlight(bx, by, color_text);

	pct = (osd_type == OSD_VOLUME) ? cur_vol : cur_bright;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;

	sx = bx + icon_size + spacing;
	rem = (100 % step_percentage != 0);
	total_sq = 100 / step_percentage + rem;
	active_sq = (pct + step_percentage - 1) / step_percentage;
	if (active_sq > total_sq)
		active_sq = total_sq;

	for (i = 0; i < total_sq; i++) {
		col = (osd_type == OSD_VOLUME && cur_mute) ? color_disabled : ((i < active_sq) ? color_accent : color_inactive_accent);
		sq_x = sx + i * (square_width + square_gap);
		fill(pixel_data, popup_width, popup_height, sq_x, border_width + (bg_h - square_height) / 2, square_width, square_height, col);
	}

	tx = sx + total_sq * (square_width + square_gap) - square_gap + spacing;
	ty = border_width + (bg_h - FONT_HEIGHT) / 2;
	if (osd_type == OSD_VOLUME && cur_mute)
		snprintf(s, sizeof(s), "mute");
	else
		snprintf(s, sizeof(s), "%3d%%", pct);
	draw_string(tx, ty, s, (osd_type == OSD_VOLUME && cur_mute) ? color_disabled : color_text);

	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage(surf, 0, 0, popup_width, popup_height);
	wl_surface_commit(surf);
}

static void
draw_arc(double cx, double cy, double r, int arc, uint32_t c, double th)
{
	double a, da, dist, dt, max_a, min_a, px, py, x, y;
	int x0, x1, y0, y1;

	min_a = (arc == 1) ? -0.55 : (arc == 2) ? -0.65 : -0.75;
	max_a = -min_a;
	x0 = (int)floor(cx - r - th);
	x1 = (int)ceil(cx + r + th);
	y0 = (int)floor(cy - r - th);
	y1 = (int)ceil(cy + r + th);

	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			px = x + 0.5;
			py = y + 0.5;
			dist = hypot(px - cx, py - cy);
			dt = fabs(dist - r);
			if (dt > th + 0.5)
				continue;
			a = atan2(py - cy, px - cx);
			da = (a < min_a) ? min_a - a : ((a > max_a) ? a - max_a : 0.0);
			if (da > 0.3)
				continue;
			blend_pixel((int)x, (int)y, c, (int)((1.0 - dt / (th + 0.5)) * (1.0 - da / 0.3) * 255));
		}
	}
}

static void
draw_backlight(int x, int y, uint32_t c)
{
	static const double angles[8] = {
		0.0, 0.785398, 1.570796, 2.356194,
		3.141593, 3.926991, 4.712389, 5.497787
	};
	double cx, cy, d, r, x0, x1, y0, y1;
	int i, px, py;

	cx = x + 10.0;
	cy = y + 10.0;
	for (py = y + 5; py <= y + 15; py++) {
		for (px = x + 5; px <= x + 15; px++) {
			d = hypot(px + 0.5 - cx, py + 0.5 - cy);
			if (d < 4.0)
				blend_pixel(px, py, c, 255);
			else if (d < 5.0)
				blend_pixel(px, py, c, (int)((5.0 - d) * 255));
		}
	}
	r = 6.2;
	for (py = y + 3; py <= y + 17; py++) {
		for (px = x + 3; px <= x + 17; px++) {
			d = fabs(hypot(px + 0.5 - cx, py + 0.5 - cy) - r);
			if (d < 0.6)
				blend_pixel(px, py, c, (int)((1.0 - d / 0.6) * 180));
		}
	}
	for (i = 0; i < 8; i++) {
		x0 = cx + cos(angles[i]) * 7.5;
		y0 = cy + sin(angles[i]) * 7.5;
		x1 = cx + cos(angles[i]) * 9.5;
		y1 = cy + sin(angles[i]) * 9.5;
		draw_line(x0, y0, x1, y1, c, 0.6);
	}
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
draw_line(double x0, double y0, double x1, double y1, uint32_t c, double th)
{
	double d, dx, dy, l2, px, py, t, x, y;
	int min_x, max_x, min_y, max_y;

	min_x = (int)floor((x0 < x1 ? x0 : x1) - th - 1);
	max_x = (int)ceil((x0 > x1 ? x0 : x1) + th + 1);
	min_y = (int)floor((y0 < y1 ? y0 : y1) - th - 1);
	max_y = (int)ceil((y0 > y1 ? y0 : y1) + th + 1);
	dx = x1 - x0;
	dy = y1 - y0;
	l2 = dx * dx + dy * dy;

	for (y = min_y; y <= max_y; y++) {
		for (x = min_x; x <= max_x; x++) {
			px = x + 0.5;
			py = y + 0.5;
			t = l2 > 0.0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0.0;
			t = (t < 0.0) ? 0.0 : ((t > 1.0) ? 1.0 : t);
			d = hypot(px - (x0 + t * dx), py - (y0 + t * dy));
			if (d < th)
				blend_pixel((int)x, (int)y, c, (int)((1.0 - d / th) * 255));
		}
	}
}

static int
draw_string(int x, int y, const char *s, uint32_t c)
{
	int orig_x;

	orig_x = x;
	while (*s) {
		draw_char(x, y, (unsigned char)*s, c);
		x += FONT_WIDTH;
		s++;
	}
	return x - orig_x;
}

static void
draw_volume(int x, int y, uint32_t c)
{
	double cx, cy, d, px, py;
	int i, j, p_x, p_y;

	for (p_y = y + 3; p_y <= y + 17; p_y++) {
		for (p_x = x + 1; p_x <= x + 8; p_x++) {
			px = p_x + 0.5;
			py = p_y + 0.5;
			d = 0.0;
			for (i = 0; i < 2; i++) {
				for (j = 0; j < 2; j++) {
					if (point_in_poly(px + (i ? 0.25 : -0.25) - x, py + (j ? 0.25 : -0.25) - y, spk, 6))
						d += 0.25;
				}
			}
			if (d > 0.0)
				blend_pixel(p_x, p_y, c, (int)(d * 255));
		}
	}

	if (cur_mute) {
		draw_line(x + 9.5, y + 6.5, x + 16.5, y + 13.5, c, 0.9);
		draw_line(x + 16.5, y + 6.5, x + 9.5, y + 13.5, c, 0.9);
		return;
	}

	cx = x + 6.0;
	cy = y + 10.0;
	if (cur_vol > 0)
		draw_arc(cx, cy, 4.8, 1, c, 0.7);
	if (cur_vol > 33)
		draw_arc(cx, cy, 8.2, 2, c, 0.7);
	if (cur_vol > 66)
		draw_arc(cx, cy, 11.6, 3, c, 0.7);
}

static void
fill(uint32_t *p, int w, int h, int x, int y, int rw, int rh, uint32_t col)
{
	int i, j, x1, y1;

	x1 = x + rw;
	y1 = y + rh;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;
	for (j = y; j < y1; j++) {
		for (i = x; i < x1; i++)
			p[j * w + i] = col;
	}
}

static void
ls_cfg(void *d, struct zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h)
{
	(void)d;
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(s, serial);
	configured = 1;
	if (visible)
		draw();
}

static void
ls_closed(void *d, struct zwlr_layer_surface_v1 *s)
{
	(void)d;
	(void)s;
	destroy_surface();
}

static int
point_in_poly(double px, double py, const double pts[][2], int n)
{
	double x0, x1, y0, y1;
	int c, i, j;

	c = 0;
	for (i = 0, j = n - 1; i < n; j = i++) {
		x0 = pts[j][0];
		y0 = pts[j][1];
		x1 = pts[i][0];
		y1 = pts[i][1];
		if (((y1 > py) != (y0 > py)) && (px < (x0 - x1) * (py - y1) / (y0 - y1) + x1))
			c = !c;
	}
	return c;
}

static void
pulse_disconnect(void)
{
	if (pulse_fd >= 0)
		close(pulse_fd);
	pulse_fd = -1;
}

static int
pulse_send_cmd(int fd, uint32_t cmd, uint32_t tag, const uint8_t *body, size_t body_len)
{
	uint8_t buf[512];
	uint32_t plen;

	plen = 10 + body_len;
	buf[0] = (plen >> 24) & 0xff;
	buf[1] = (plen >> 16) & 0xff;
	buf[2] = (plen >> 8) & 0xff;
	buf[3] = plen & 0xff;
	buf[4] = buf[5] = buf[6] = buf[7] = 0xff;
	buf[8] = buf[9] = buf[10] = buf[11] = 0;
	buf[12] = buf[13] = buf[14] = buf[15] = 0;
	buf[16] = buf[17] = buf[18] = buf[19] = 0;

	buf[20] = 'L';
	buf[21] = (cmd >> 24) & 0xff;
	buf[22] = (cmd >> 16) & 0xff;
	buf[23] = (cmd >> 8) & 0xff;
	buf[24] = cmd & 0xff;

	buf[25] = 'L';
	buf[26] = (tag >> 24) & 0xff;
	buf[27] = (tag >> 16) & 0xff;
	buf[28] = (tag >> 8) & 0xff;
	buf[29] = tag & 0xff;

	if (body_len > 0)
		memcpy(&buf[30], body, body_len);

	return send(fd, buf, 20 + plen, MSG_NOSIGNAL) == (ssize_t)(20 + plen) ? 0 : -1;
}

static int
pulse_recv_packet(int fd, uint8_t *buf, size_t max_len)
{
	uint8_t hdr[20];
	size_t len, off;
	ssize_t n;

	off = 0;
	while (off < 20) {
		if ((n = recv(fd, hdr + off, 20 - off, 0)) <= 0)
			return -1;
		off += n;
	}
	len = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | (size_t)hdr[3];
	off = 0;
	while (off < len) {
		size_t to_read = len - off;
		if (to_read > max_len - off)
			to_read = max_len - off;
		if ((n = recv(fd, buf + off, to_read, 0)) <= 0)
			return -1;
		off += n;
	}
	return len <= max_len ? (int)len : (int)max_len;
}

static int
pulse_recv_reply(int fd, uint32_t req_tag, uint8_t *buf, size_t max_len)
{
	int len;
	uint32_t cmd, tag;

	while ((len = pulse_recv_packet(fd, buf, max_len)) >= 10) {
		if (buf[0] != 'L' || buf[5] != 'L')
			continue;
		cmd = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 8) | buf[4];
		tag = ((uint32_t)buf[6] << 24) | ((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 8) | buf[9];
		if (tag == req_tag)
			return (cmd == 2) ? len : -1;
	}
	return -1;
}

static int
pulse_connect(void)
{
	struct sockaddr_un sa;
	struct timeval tv;
	uint8_t cookie[256], body[270], resp[1024];
	const char *rundir, *home;
	char path[256];
	int cfd, fd;

	static const uint8_t prop[] = {
		'P', 't', 'a', 'p', 'p', 'l', 'i', 'c', 'a', 't', 'i', 'o', 'n', '.', 'n', 'a', 'm', 'e', 0,
		'L', 0, 0, 0, 4, 'x', 0, 0, 0, 4, 'o', 's', 'd', 0, 'N'
	};
	static const uint8_t sub_req[] = {
		'L', 0, 0, 2, 0xff
	};

	pulse_disconnect();
	rundir = getenv("XDG_RUNTIME_DIR");
	if (!rundir)
		rundir = "/run/user/1000";
	if (snprintf(path, sizeof(path), "%s/pulse/native", rundir) >= (int)sizeof(sa.sun_path))
		return -1;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	tv.tv_sec = 0;
	tv.tv_usec = 250000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, path, strlen(path) + 1);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}

	home = getenv("HOME");
	if (!home)
		home = "/home/felix";
	if ((size_t)snprintf(path, sizeof(path), "%s/.config/pulse/cookie", home) >= sizeof(path)) {
		close(fd);
		return -1;
	}
	cfd = open(path, O_RDONLY);
	if (cfd < 0 || read(cfd, cookie, 256) != 256) {
		if (cfd >= 0)
			close(cfd);
		close(fd);
		return -1;
	}
	close(cfd);

	body[0] = 'L';
	body[1] = 0; body[2] = 0; body[3] = 0; body[4] = 35;
	body[5] = 'x';
	body[6] = 0; body[7] = 0; body[8] = 1; body[9] = 0;
	memcpy(&body[10], cookie, 256);

	if (pulse_send_cmd(fd, 8, pulse_tag, body, 266) < 0 || pulse_recv_reply(fd, pulse_tag++, resp, sizeof(resp)) < 0) {
		close(fd);
		return -1;
	}
	if (pulse_send_cmd(fd, 9, pulse_tag, prop, sizeof(prop)) < 0 || pulse_recv_reply(fd, pulse_tag++, resp, sizeof(resp)) < 0) {
		close(fd);
		return -1;
	}
	if (pulse_send_cmd(fd, 35, pulse_tag, sub_req, sizeof(sub_req)) < 0 || pulse_recv_reply(fd, pulse_tag++, resp, sizeof(resp)) < 0) {
		close(fd);
		return -1;
	}

	pulse_fd = fd;
	return 0;
}

static int
read_sysfs(const char *base, const char *entry)
{
	char b[32], p[256];
	int fd, n, v;

	if ((size_t)snprintf(p, sizeof(p), "%s/%s", base, entry) >= sizeof(p))
		return -1;
	if ((fd = open(p, O_RDONLY)) < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	b[n] = '\0';
	v = atoi(b);
	return v >= 0 ? v : -1;
}

static void
reg_glob(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v)
{
	(void)d;
	(void)v;
	if (!strcmp(i, wl_compositor_interface.name))
		comp = wl_registry_bind(r, n, &wl_compositor_interface, 4);
	else if (!strcmp(i, wl_shm_interface.name))
		shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
	else if (!strcmp(i, zwlr_layer_shell_v1_interface.name))
		shell = wl_registry_bind(r, n, &zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(i, wl_output_interface.name)) {
		if (output_count < (int)(sizeof(output_names) / sizeof(output_names[0])))
			output_names[output_count++] = n;
		if (!ls && comp && shell)
			create_surface();
	}
}

static void
reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	int i;

	(void)d;
	(void)r;
	for (i = 0; i < output_count; i++) {
		if (output_names[i] == n) {
			output_names[i] = output_names[--output_count];
			break;
		}
	}
	if (!output_count && ls)
		destroy_surface();
}

static void
set_visibility(int v)
{
	if (visible == v)
		return;
	visible = v;
	if (!visible && surf && configured) {
		fill(pixel_data, popup_width, popup_height, 0, 0, popup_width, popup_height, 0);
		wl_surface_attach(surf, buf, 0, 0);
		wl_surface_damage(surf, 0, 0, popup_width, popup_height);
		wl_surface_commit(surf);
	}
}

static void
setup(void)
{
	struct sockaddr_nl sa;
	struct wl_shm_pool *p;
	int fd, sz;

	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);

	if (!(dpy = wl_display_connect(NULL)))
		die("failed to connect to Wayland display");
	if (!(reg = wl_display_get_registry(dpy)))
		die("failed to get Wayland registry");
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);

	if (!comp || !shm || !shell)
		die("missing required Wayland globals");

	sz = popup_width * popup_height * 4;
	if ((fd = memfd_create("osd", MFD_CLOEXEC)) < 0)
		die("failed to create shm file");
	if (ftruncate(fd, sz) < 0)
		die("failed to truncate shm");
	pixel_data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixel_data == MAP_FAILED)
		die("failed to mmap shm");

	p = wl_shm_create_pool(shm, fd, sz);
	buf = wl_shm_pool_create_buffer(p, 0, popup_width, popup_height, popup_width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(p);
	close(fd);

	if (output_count > 0)
		create_surface();
	wl_display_roundtrip(dpy);

	if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) < 0)
		die("failed to create timerfd");

	if ((netlink_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT)) >= 0) {
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
		sa.nl_groups = 1;
		if (bind(netlink_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(netlink_fd);
			netlink_fd = -1;
		}
	}
}

static void
sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

static int
update_sysfs(void)
{
	int cur, max, v;

	cur = read_sysfs(backlight_path, "brightness");
	max = read_sysfs(backlight_path, "max_brightness");
	v = (cur < 0 || max <= 0) ? 0 : (cur * 100 + max / 2) / max;
	if (v == cur_bright)
		return 0;
	cur_bright = v;
	return 1;
}

static int
update_volume(void)
{
	uint8_t resp[4096];
	int chans, len, mute, v, v_idx;
	uint32_t raw_vol, tag;

	static const uint8_t sink_req[] = {
		'L', 0xff, 0xff, 0xff, 0xff,
		't', '@', 'D', 'E', 'F', 'A', 'U', 'L', 'T', '_', 'S', 'I', 'N', 'K', '@', 0
	};

	if (pulse_fd < 0 && pulse_connect() < 0)
		return 0;
	tag = pulse_tag++;
	if (pulse_send_cmd(pulse_fd, 21, tag, sink_req, sizeof(sink_req)) < 0) {
		pulse_disconnect();
		return 0;
	}
	if ((len = pulse_recv_reply(pulse_fd, tag, resp, sizeof(resp))) < 0) {
		pulse_disconnect();
		return 0;
	}

	v = -1;
	mute = 0;
	for (v_idx = 0; v_idx < len - 10; v_idx++) {
		if (resp[v_idx] == 'v') {
			chans = resp[v_idx + 1];
			raw_vol = ((uint32_t)resp[v_idx + 2] << 24) |
			          ((uint32_t)resp[v_idx + 3] << 16) |
			          ((uint32_t)resp[v_idx + 4] << 8)  |
			           (uint32_t)resp[v_idx + 5];
			v = (raw_vol * 100 + 32768) / 65536;
			if (v > 100)
				v = 100;
			if (v_idx + 2 + 4 * chans < len)
				mute = (resp[v_idx + 2 + 4 * chans] == '1');
			break;
		}
	}
	if (v < 0)
		return 0;
	if (v == cur_vol && mute == cur_mute)
		return 0;
	cur_vol = v;
	cur_mute = mute;
	return 1;
}

static void
usage(void)
{
	die("usage: osd [-b pct] [-c corner] [-h] [-t pct] [-v]");
}

int
main(int argc, char **argv)
{
	struct pollfd pfds[4];
	uint64_t exp;
	char nlk[512];
	int initialized, nfds, opt, test_mode, test_pct, test_type;

	selected_corner = corner;
	test_mode = 0;
	test_pct = -1;
	test_type = OSD_VOLUME;
	while ((opt = getopt(argc, argv, "b:c:ht:v")) >= 0) {
		switch (opt) {
		case 'b':
			test_pct = atoi(optarg);
			test_type = OSD_BRIGHTNESS;
			test_mode = 1;
			break;
		case 'c':
			selected_corner = (optarg[0] >= '0' && optarg[0] <= '3') ? optarg[0] - '0' : ((optarg[0] == 'b') << 1) | (optarg[1] == 'r');
			break;
		case 't':
			test_pct = atoi(optarg);
			test_type = OSD_VOLUME;
			test_mode = 1;
			break;
		case 'v':
			puts("osd-" VERSION);
			return 0;
		default:
			usage();
		}
	}

	setup();
	if (test_mode) {
		osd_type = test_type;
		if (test_type == OSD_VOLUME)
			cur_vol = test_pct;
		else
			cur_bright = test_pct;
		visible = 1;
		arm_timer(hide_time);
		draw();
	} else {
		update_sysfs();
		update_volume();
		initialized = 1;
	}

	memset(pfds, 0, sizeof(pfds));
	pfds[0].fd = wl_display_get_fd(dpy);
	pfds[0].events = POLLIN;
	pfds[1].fd = timer_fd;
	pfds[1].events = POLLIN;
	pfds[2].fd = netlink_fd;
	pfds[2].events = POLLIN;

	while (running) {
		if (!ls && output_count > 0)
			create_surface();
		if (!test_mode && pulse_fd < 0)
			update_volume();
		pfds[3].fd = pulse_fd;
		pfds[3].events = POLLIN;
		nfds = (pulse_fd >= 0) ? 4 : 3;

		while (wl_display_prepare_read(dpy) < 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		if (poll(pfds, nfds, -1) < 0) {
			wl_display_cancel_read(dpy);
			running &= (errno == EINTR);
			continue;
		}

		if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				break;
		} else {
			wl_display_cancel_read(dpy);
		}
		if (wl_display_dispatch_pending(dpy) < 0)
			break;

		if (timer_fd >= 0 && (pfds[1].revents & POLLIN) && read(timer_fd, &exp, sizeof(exp)) > 0) {
			set_visibility(0);
			if (test_mode)
				running = 0;
		}

		if (!test_mode && netlink_fd >= 0 && (pfds[2].revents & POLLIN)) {
			while (recv(netlink_fd, nlk, sizeof(nlk), 0) > 0) {}
			if (update_sysfs() && initialized) {
				osd_type = OSD_BRIGHTNESS;
				visible = 1;
				arm_timer(hide_time);
				draw();
			}
		}

		if (!test_mode && pulse_fd >= 0 && (pfds[3].revents & (POLLIN | POLLERR | POLLHUP))) {
			if (pfds[3].revents & (POLLERR | POLLHUP)) {
				pulse_disconnect();
			} else if (update_volume() && initialized) {
				osd_type = OSD_VOLUME;
				visible = 1;
				arm_timer(hide_time);
				draw();
			}
		}
	}

	cleanup();
	return 0;
}
