#define _GNU_SOURCE
#include <dirent.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

const struct wl_interface xdg_popup_interface;

#define POLL_SECS  20
#define LEVEL_LOW  15
#define LEVEL_CRIT 5

static char bat_dir[320];
static int  ui_scale = 1, grab_keyboard = 1, test_pct = -1, done;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *shell;
static struct wl_surface *surf;
static struct wl_buffer *buf;

static int read_sysfs(const char *sub, char *buf)
{
	char p[384];
	snprintf(p, sizeof p, "%s/%s", bat_dir, sub);
	FILE *f = fopen(p, "r");
	if (!f) return -1;
	int r = fscanf(f, "%15s", buf);
	fclose(f);
	return r == 1 ? 0 : -1;
}

static int find_battery(void)
{
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return -1;
	struct dirent *e;
	char b[16];
	while ((e = readdir(d))) {
		snprintf(bat_dir, sizeof bat_dir, "/sys/class/power_supply/%s", e->d_name);
		if (!read_sysfs("type", b) && !strcmp(b, "Battery")) {
			closedir(d);
			return 0;
		}
	}
	*bat_dir = 0;
	closedir(d);
	return -1;
}

static int battery_state(int *cap, int *discharging)
{
	char b[16];
	if (read_sysfs("capacity", b)) return -1;
	*cap = atoi(b);
	if (read_sysfs("status", b)) return -1;
	*discharging = (b[0] == 'D');
	return 0;
}

static const struct { char c; uint8_t r[7]; } font[] = {
	{'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
	{'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
	{'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
	{'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
	{'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
	{'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
	{'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
	{'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
	{'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
	{'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
	{'%', {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}},
	{'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
	{'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
	{'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
	{'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
	{'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
	{'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
	{'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
	{'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
	{'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
};

static void fill(uint32_t *px, int W, int H, int x, int y, int w, int h, uint32_t col)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++)
			px[j * W + i] = col;
}

static void draw_text(uint32_t *px, int W, int H, int cx, int y, const char *s, int sc, uint32_t col)
{
	int x = cx - (strlen(s) * 6 * sc - sc) / 2;
	for (; *s; s++, x += 6 * sc) {
		for (size_t i = 0; i < sizeof font / sizeof *font; i++) {
			if (font[i].c != *s) continue;
			for (int r = 0; r < 7; r++)
				for (int c = 0; c < 5; c++)
					if (font[i].r[r] & (0x10 >> c))
						fill(px, W, H, x + c * sc, y + r * sc, sc, sc, col);
			break;
		}
	}
}

static void render(uint32_t *px, int W, int H, int cap, int level)
{
	int u = ui_scale, b = 3 * u, cx = W / 2;
	uint32_t accent = level <= LEVEL_CRIT ? 0xFFF38BA8 : 0xFFFAB387;
	char pct[16];

	fill(px, W, H, 0, 0, W, H, accent);
	fill(px, W, H, b, b, W - 2 * b, H - 2 * b, 0xFF1E1E2E);
	draw_text(px, W, H, cx, 20 * u, level <= LEVEL_CRIT ? "BATTERY CRITICAL" : "BATTERY LOW", 4 * u, 0xFFCDD6F4);
	snprintf(pct, sizeof pct, "%d%%", cap);
	draw_text(px, W, H, cx, 62 * u, pct, 8 * u, accent);
	draw_text(px, W, H, cx, 132 * u, "PLUG IN CHARGER", 3 * u, 0xFFA6ADC8);
}

static void noop(void) {}
#define N ((void (*))noop)

static void ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) { if (st) done = 1; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) { if (st) done = 1; }
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) { close(fd); }
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls) { done = 1; }

static const struct wl_pointer_listener ptr_l = {
	N, N, N, ptr_btn, N, N, N, N, N
};
static const struct wl_keyboard_listener kb_l = {
	kb_keymap, N, N, kb_key, N, N
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
	if (caps & WL_SEAT_CAPABILITY_POINTER)
		wl_pointer_add_listener(wl_seat_get_pointer(seat), &ptr_l, NULL);
	if (grab_keyboard && (caps & WL_SEAT_CAPABILITY_KEYBOARD))
		wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &kb_l, NULL);
}

static const struct wl_seat_listener seat_l = { seat_caps, N };

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
}

static const struct zwlr_layer_surface_v1_listener ls_l = { ls_configure, ls_closed };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 1);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name))
		wl_seat_add_listener(wl_registry_bind(r, name, &wl_seat_interface, 1), &seat_l, NULL);
}

static const struct wl_registry_listener reg_l = { reg_global, N };

static int show_popup(int cap, int level)
{
	int W = 440 * ui_scale, H = 176 * ui_scale, ret = -1;
	size_t size = (size_t)W * 4 * H;
	uint32_t *px = MAP_FAILED;
	struct zwlr_layer_surface_v1 *ls = NULL;

	done = 0;
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) return -1;

	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !shell) goto out;

	int fd = memfd_create("lowbat", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); goto out; }
	px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) { close(fd); goto out; }
	render(px, W, H, cap, level);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buf = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	surf = wl_compositor_create_surface(comp);
	ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, NULL,
	        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "lowbat");
	zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
	zwlr_layer_surface_v1_set_size(ls, W, H);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
	if (grab_keyboard)
		zwlr_layer_surface_v1_set_keyboard_interactivity(ls,
		        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(surf);

	struct pollfd pfd = { wl_display_get_fd(dpy), POLLIN, 0 };
	while (!done) {
		while (wl_display_prepare_read(dpy))
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);
		if (poll(&pfd, 1, 500) > 0)
			wl_display_read_events(dpy);
		else
			wl_display_cancel_read(dpy);
		wl_display_dispatch_pending(dpy);

		int c, dis;
		if (test_pct < 0 && !battery_state(&c, &dis) && !dis) break;
	}
	ret = 0;
out:
	if (ls) zwlr_layer_surface_v1_destroy(ls);
	if (surf) wl_surface_destroy(surf);
	if (buf) wl_buffer_destroy(buf);
	if (px != MAP_FAILED) munmap(px, size);
	wl_display_disconnect(dpy);
	comp = NULL; shm = NULL; shell = NULL;
	return ret;
}

int main(int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "s:nt:")) != -1) {
		if (opt == 's') ui_scale = atoi(optarg) < 1 ? 1 : atoi(optarg);
		else if (opt == 'n') grab_keyboard = 0;
		else if (opt == 't') test_pct = atoi(optarg);
		else return 1;
	}

	if (test_pct >= 0)
		return show_popup(test_pct, test_pct <= LEVEL_CRIT ? LEVEL_CRIT : LEVEL_LOW) != 0;

	int shown = 101, cap, dis;
	for (;;) {
		if (!*bat_dir && find_battery() < 0) {
			sleep(POLL_SECS);
			continue;
		}
		if (battery_state(&cap, &dis)) {
			*bat_dir = 0;
		} else if (!dis) {
			shown = 101;
		} else {
			int want = cap <= LEVEL_CRIT ? LEVEL_CRIT : cap <= LEVEL_LOW ? LEVEL_LOW : 0;
			if (want && want < shown && !show_popup(cap, want))
				shown = want;
		}
		sleep(POLL_SECS);
	}
}
