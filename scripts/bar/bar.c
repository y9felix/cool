#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
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

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define PI 3.14159265358979323846
#define KAPPA 0.552285
#define LEVEL(v, m) ((v) <= 0 ? 0 : (v) >= 100 ? (m) : ((v) * (m) + 50) / 100)
#define SET_RGBA(cr, c) cairo_set_source_rgba(cr, (c)[0], (c)[1], (c)[2], (c)[3])
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
static cairo_surface_t *back_surf;
static cairo_t *back_cr;
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

static void
set_brightness_delta(int delta)
{
	system(delta > 0 ? "brightnessctl -q set 5%+ >/dev/null 2>&1" : "brightnessctl -q set 5%- >/dev/null 2>&1");
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
set_volume_delta(int delta)
{
	long min, max, vol, step;

	if (!mixer_elem)
		return;
	snd_mixer_selem_get_playback_volume_range(mixer_elem, &min, &max);
	snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_MONO, &vol);
	step = (max - min) / 20;
	vol += delta * (step > 0 ? step : 1);
	snd_mixer_selem_set_playback_volume_all(mixer_elem, vol < min ? min : vol > max ? max : vol);
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
rounded_rect(cairo_t *cr, double x0, double y0, double x1, double y1, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x1 - r, y0 + r, r, -PI / 2, 0);
	cairo_arc(cr, x1 - r, y1 - r, r, 0, PI / 2);
	cairo_arc(cr, x0 + r, y1 - r, r, PI / 2, PI);
	cairo_arc(cr, x0 + r, y0 + r, r, PI, 3 * PI / 2);
	cairo_close_path(cr);
}

static void
draw_icon_brightness(cairo_t *cr, int lvl)
{
	double ax, c, r = 0.2975;

	cairo_new_sub_path(cr);
	cairo_arc(cr, 0.5, 0.5, 0.34, 0, 2 * PI);
	cairo_set_line_width(cr, 0.085);
	cairo_stroke(cr);
	if (lvl > 0) {
		ax = r * (lvl / 3.0 - 1.0);
		c = KAPPA * r;
		cairo_move_to(cr, 0.5, 0.5 - r);
		cairo_curve_to(cr, 0.5 - c, 0.5 - r, 0.5 - r, 0.5 - c, 0.5 - r, 0.5);
		cairo_curve_to(cr, 0.5 - r, 0.5 + c, 0.5 - c, 0.5 + r, 0.5, 0.5 + r);
		cairo_curve_to(cr, 0.5 + KAPPA * ax, 0.5 + r, 0.5 + ax, 0.5 + c, 0.5 + ax, 0.5);
		cairo_curve_to(cr, 0.5 + ax, 0.5 - c, 0.5 + KAPPA * ax, 0.5 - r, 0.5, 0.5 - r);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
}

static void
draw_icon_volume(cairo_t *cr, int lvl, int muted)
{
	int i;

	cairo_move_to(cr, 0.06, 0.38);
	cairo_line_to(cr, 0.21, 0.38);
	cairo_line_to(cr, 0.36, 0.18);
	cairo_line_to(cr, 0.36, 0.82);
	cairo_line_to(cr, 0.21, 0.62);
	cairo_line_to(cr, 0.06, 0.62);
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
			cairo_set_line_width(cr, i < lvl ? 0.085 : 0.028);
			cairo_new_sub_path(cr);
			cairo_arc(cr, 0.34, 0.50, 0.14 + i * 0.14, -0.84, 0.84);
			cairo_stroke(cr);
		}
	}
}

static void
draw_icon_battery(cairo_t *cr, int lvl, int charging)
{
	double filled;

	rounded_rect(cr, 0.04, 0.188, 1.07, 0.812, 0.08);
	cairo_set_line_width(cr, 0.08);
	cairo_stroke(cr);
	rounded_rect(cr, 1.12, 0.40, 1.23, 0.60, 0.03);
	cairo_fill(cr);
	filled = 0.10 + 0.91 * (lvl + 1) / 6.0;
	rounded_rect(cr, 0.10, 0.248, filled, 0.752, 0.03);
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
}

static void
draw_icon_clock(cairo_t *cr)
{
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width(cr, 0.08);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 0.5, 0.5, 0.34, 0, 2 * PI);
	cairo_stroke(cr);
	cairo_move_to(cr, 0.5, 0.28);
	cairo_line_to(cr, 0.5, 0.5);
	cairo_line_to(cr, 0.66, 0.58);
	cairo_stroke(cr);
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
	cairo_font_extents_t fext;
	cairo_text_extents_t text;
	char mod_text[32];
	int icon_y, iw, x, y;
	size_t i;

	if (!back_cr || !configured)
		return;
	if (opt_autohide && !visible) {
		memset(pixel_data, 0, buffer_size);
	} else {
		cairo_new_path(back_cr);
		cairo_set_operator(back_cr, CAIRO_OPERATOR_SOURCE);
		SET_RGBA(back_cr, color_bg);
		cairo_paint(back_cr);
		cairo_set_operator(back_cr, CAIRO_OPERATOR_OVER);

		x = padding_x;
		icon_y = (bar_height - icon_size) / 2;
		if (icon_y < 0)
			icon_y = 0;
		cairo_font_extents(back_cr, &fext);
		y = (bar_height - (int)(fext.height + 0.5)) / 2 + (int)(fext.ascent + 0.5);

		for (i = 0; i < 4; i++) {
			SET_RGBA(back_cr, color_fg);
			iw = icon_size;
			cairo_new_path(back_cr);
			cairo_save(back_cr);
			cairo_translate(back_cr, x, icon_y);
			cairo_scale(back_cr, icon_size, icon_size);
			switch (i) {
			case 0:
				draw_icon_brightness(back_cr, LEVEL(cur_bri, 6));
				snprintf(mod_text, sizeof(mod_text), "%d%%", cur_bri < 0 ? 0 : cur_bri);
				break;
			case 1:
				if (cur_mute)
					SET_RGBA(back_cr, color_disabled);
				draw_icon_volume(back_cr, LEVEL(cur_vol, 4), cur_mute);
				snprintf(mod_text, sizeof(mod_text), "%d%%", cur_vol < 0 ? 0 : cur_vol);
				break;
			case 2:
				iw = (int)(1.25 * icon_size + 0.5);
				draw_icon_battery(back_cr, LEVEL(cur_bat, 5), cur_chg);
				snprintf(mod_text, sizeof(mod_text), "%d%%", cur_bat < 0 ? 0 : cur_bat);
				break;
			case 3:
				draw_icon_clock(back_cr);
				snprintf(mod_text, sizeof(mod_text), "%s", cur_time);
				break;
			}
			cairo_restore(back_cr);
			cairo_new_path(back_cr);
			x += iw + icon_spacing;
			cairo_text_extents(back_cr, mod_text, &text);
			cairo_move_to(back_cr, x, y);
			cairo_show_text(back_cr, mod_text);
			cairo_new_path(back_cr);
			x += (int)(text.x_advance + 0.5) + spacing;
			if (i < 2)
				bounds[i] = x;
		}
		cairo_surface_flush(back_surf);
		memcpy(pixel_data, cairo_image_surface_get_data(back_surf), buffer_size);
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
ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t val)
{
	int dlt;

	if (val == 0)
		return;
	dlt = val < 0 ? 1 : -1;
	if (pointer_x < bounds[0]) {
		set_brightness_delta(dlt);
		if (update_brightness() && visible)
			draw();
	} else if (pointer_x < bounds[1]) {
		set_volume_delta(dlt);
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
	.button = ptr_btn, .axis = ptr_axis
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

	back_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bar_width, bar_height);
	back_cr = cairo_create(back_surf);
	cairo_select_font_face(back_cr, font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(back_cr, font_size);
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
	if (back_cr)
		cairo_destroy(back_cr);
	if (back_surf)
		cairo_surface_destroy(back_surf);
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
