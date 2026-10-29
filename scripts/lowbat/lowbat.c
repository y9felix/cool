#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "config.h"

#define N noop
#define BIND(iface) wl_registry_bind(r, n, &(iface), 1)

static void noop();
static void ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t,
    uint32_t b, uint32_t st);
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t,
    uint32_t key, uint32_t st);
static void kb_map(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd,
    uint32_t sz);
static void ls_cfg(void *d, struct zwlr_layer_surface_v1 *l, uint32_t s,
    uint32_t w, uint32_t h);
static void seat_caps(void *d, struct wl_seat *s, uint32_t c);
static void reg_glob(void *d, struct wl_registry *r, uint32_t n,
    const char *i, uint32_t v);

const struct wl_interface xdg_popup_interface;

static const struct { char c; uint8_t r[7]; } font[] = {
	{ '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
	{ '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
	{ '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
	{ '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
	{ '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
	{ '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
	{ '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
	{ '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
	{ '%', { 0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13 } },
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } }
};

static int scale = default_scale, grab_kbd = default_grab, test_pct = -1, done;
static void *comp, *shm, *shell, *surf, *buf;

static const struct wl_pointer_listener ptr_l = {
	N, N, N, ptr_btn, N, N, N, N, N, N, N, N
};
static const struct wl_keyboard_listener kb_l = { kb_map, N, N, kb_key, N, N };
static const struct zwlr_layer_surface_v1_listener ls_l = { ls_cfg, N };
static const struct wl_seat_listener seat_l = { seat_caps, N };
static const struct wl_registry_listener reg_l = { reg_glob, N };

static void
noop()
{
}

static int
read_sysfs(const char *sub, char *b)
{
	char p[80];
	int fd, n;

	snprintf(p, sizeof(p), "%s/%s", bat_path, sub);
	if ((fd = open(p, O_RDONLY)) < 0)
		return -1;
	n = read(fd, b, 15);
	close(fd);
	if (n <= 0)
		return -1;
	b[n] = '\0';
	return 0;
}

static int
battery_state(int *cap, int *dis)
{
	char b[16];

	if (read_sysfs("capacity", b) < 0)
		return -1;
	*cap = atoi(b);
	if (read_sysfs("status", b) < 0)
		return -1;
	*dis = (b[0] == 'D');
	return 0;
}

static void
fill(uint32_t *p, int w, int h, int x, int y, int rw, int rh, uint32_t col)
{
	int i, j, x1 = x + rw > w ? w : x + rw, y1 = y + rh > h ? h : y + rh;

	for (j = y < 0 ? 0 : y; j < y1; j++) {
		for (i = x < 0 ? 0 : x; i < x1; i++)
			p[j * w + i] = col;
	}
}

static void
draw_text(uint32_t *p, int w, int h, int cx, int y, const char *s, int sc,
    uint32_t col)
{
	int x = cx - ((int)strlen(s) * 6 * sc - sc) / 2, r, c;
	size_t i;

	for (; *s; s++, x += 6 * sc) {
		for (i = 0; i < sizeof(font) / sizeof(*font); i++) {
			if (font[i].c != *s)
				continue;
			for (r = 0; r < 7; r++) {
				for (c = 0; c < 5; c++)
					if (font[i].r[r] & (0x10 >> c))
						fill(p, w, h, x + c * sc,
						    y + r * sc, sc, sc, col);
			}
			break;
		}
	}
}

static void
render(uint32_t *p, int w, int h, int cap, int level)
{
	char pct[16];
	int b = border_width * scale, cx = w / 2, c = level <= level_crit;
	uint32_t col = c ? color_crit_accent : color_low_accent;

	fill(p, w, h, 0, 0, w, h, col);
	fill(p, w, h, b, b, w - 2 * b, h - 2 * b, color_bg);
	draw_text(p, w, h, cx, 20 * scale, c ? msg_critical : msg_low,
	    4 * scale, color_text);
	snprintf(pct, sizeof(pct), "%d%%", cap);
	draw_text(p, w, h, cx, 62 * scale, pct, 8 * scale, col);
	draw_text(p, w, h, cx, 132 * scale, msg_action, 3 * scale,
	    color_text);
}

static void
ptr_btn(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b,
    uint32_t st)
{
	if (st)
		done = 1;
}

static void
kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key,
    uint32_t st)
{
	if (st)
		done = 1;
}

static void
kb_map(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz)
{
	close(fd);
}

static void
ls_cfg(void *d, struct zwlr_layer_surface_v1 *l, uint32_t s, uint32_t w,
    uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(l, s);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_commit(surf);
}

static void
seat_caps(void *d, struct wl_seat *s, uint32_t c)
{
	if (c & WL_SEAT_CAPABILITY_POINTER)
		wl_pointer_add_listener(wl_seat_get_pointer(s), &ptr_l, NULL);
	if (grab_kbd && (c & WL_SEAT_CAPABILITY_KEYBOARD))
		wl_keyboard_add_listener(wl_seat_get_keyboard(s), &kb_l, NULL);
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
show_popup(int cap, int level)
{
	struct wl_display *dpy;
	struct zwlr_layer_surface_v1 *ls;
	struct wl_shm_pool *pool;
	struct pollfd pfd;
	uint32_t *px;
	char b[16];
	int w = popup_width * scale, h = popup_height * scale, fd, ret = -1;
	size_t sz = (size_t)w * 4 * h;

	done = 0;
	if (!(dpy = wl_display_connect(NULL)))
		return -1;
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !shell)
		goto out;
	if ((fd = shm_open("/lowbat", O_CREAT | O_EXCL | O_RDWR, 0600)) < 0)
		goto out;
	shm_unlink("/lowbat");
	if (ftruncate(fd, sz) < 0 || (px = mmap(NULL, sz, PROT_READ |
	    PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		close(fd);
		goto out;
	}
	render(px, w, h, cap, level);
	pool = wl_shm_create_pool(shm, fd, sz);
	buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
	    WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	surf = wl_compositor_create_surface(comp);
	ls = zwlr_layer_shell_v1_get_layer_surface(shell, surf, NULL,
	    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "lowbat");
	zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
	zwlr_layer_surface_v1_set_size(ls, w, h);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
	if (grab_kbd)
		zwlr_layer_surface_v1_set_keyboard_interactivity(ls,
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(surf);
	pfd = (struct pollfd){ wl_display_get_fd(dpy), POLLIN, 0 };
	while (!done) {
		while (wl_display_prepare_read(dpy) < 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);
		if (poll(&pfd, 1, 500) > 0)
			wl_display_read_events(dpy);
		else
			wl_display_cancel_read(dpy);
		wl_display_dispatch_pending(dpy);
		if (test_pct < 0 && !read_sysfs("status", b) && b[0] != 'D')
			break;
	}
	zwlr_layer_surface_v1_destroy(ls);
	wl_surface_destroy(surf);
	wl_buffer_destroy(buf);
	munmap(px, sz);
	ret = 0;
out:
	wl_display_disconnect(dpy);
	comp = shm = shell = NULL;
	return ret;
}

int
main(int argc, char **argv)
{
	int opt, shown = 101, cap, dis, want;

	while ((opt = getopt(argc, argv, "hns:t:v")) != -1) {
		switch (opt) {
		case 's':
			if ((scale = atoi(optarg)) < 1)
				scale = 1;
			break;
		case 'n':
			grab_kbd = 0;
			break;
		case 't':
			test_pct = atoi(optarg);
			break;
		case 'v':
			puts("lowbat-" VERSION);
			return 0;
		default:
			return 1;
		}
	}
	if (test_pct >= 0)
		return show_popup(test_pct, test_pct <= level_crit ?
		    level_crit : level_low) < 0;
	for (;;) {
		if (!battery_state(&cap, &dis)) {
			want = cap <= level_crit ? level_crit :
			    cap <= level_low ? level_low : 0;
			if (!dis)
				shown = 101;
			else if (want && want < shown && !show_popup(cap, want))
				shown = want;
		}
		sleep(poll_interval);
	}
}
