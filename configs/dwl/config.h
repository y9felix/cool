#define TAGCOUNT (3)
#define MODKEY WLR_MODIFIER_CTRL
#define TAGKEYS(KEY, TAG) { MODKEY, KEY, view, {.ui = 1 << TAG} }
#define COLOR_BLACK { 0.0f, 0.0f, 0.0f, 1.0f }

static const int sloppyfocus               = 1;
static const int bypass_surface_visibility = 0;
static const unsigned int borderpx         = 0;
static const unsigned int snap             = 16;

static const float rootcolor[]     = COLOR_BLACK;
static const float bordercolor[]   = COLOR_BLACK;
static const float focuscolor[]    = COLOR_BLACK;
static const float urgentcolor[]   = COLOR_BLACK;
static const float fullscreen_bg[] = COLOR_BLACK;

static int log_level = WLR_ERROR;

static const Rule rules[] = {
	{ "none", NULL, 0, 0, -1 },
};

static const Layout layouts[] = {
	{ "[]=", tile },
};

static const MonitorRule monrules[] = {
	{ NULL, 0.55f, 1, 1, &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL, -1, -1 },
};

static const struct xkb_rule_names xkb_rules = {
	.layout  = "us,ru",
	.options = "grp:alt_shift_toggle",
};

static const int repeat_rate  = 35;
static const int repeat_delay = 180;

static const int tap_to_click            = 1;
static const int tap_and_drag            = 1;
static const int drag_lock               = 1;
static const int natural_scrolling       = 0;
static const int disable_while_typing    = 1;
static const int left_handed             = 0;
static const int middle_button_emulation = 0;
static const double accel_speed          = 0.2;

static const uint32_t send_events_mode                        = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
static const enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
static const enum libinput_config_click_method click_method   = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
static const enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
static const enum libinput_config_tap_button_map button_map   = LIBINPUT_CONFIG_TAP_MAP_LRM;

static const char *termcmd[]    = { "alacritty", NULL };
static const char *bravecmd[]   = { "brave-nightly", NULL };
static const char *fmcmd[]    = { "pcmanfm-qt", NULL };
static const char *menucmd[]    = { "wmenu-run", "-N", "000000", "-n", "FFFFFF", "-M", "000000", "-m", "FFFFFF", "-S", "7e56c3", NULL };
static const char *grimcmd[]    = { "sh", "-c", "f=\"/home/felix/pictures/screenshots/screenshot-$(date +%m.%d-%s).png\"; grim -g \"$(slurp)\" \"$f\" && wl-copy -t image/png < \"$f\"", NULL };
static const char *reccmd[] = { "sh", "-c", "wf-recorder -c h264_vaapi -d /dev/dri/renderD128 -f \"/home/felix/videos/screencasts/screencast-$(date +%m.%d-%s).mp4\"", NULL };
static const char *killreccmd[] = { "killall", "-SIGINT", "wf-recorder", NULL };
static const char *mutecmd[]    = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
static const char *voldwcmd[]   = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", NULL };
static const char *volupcmd[]   = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%+", NULL };
static const char *bldwcmd[]    = { "brightnessctl", "set", "5%-", NULL };
static const char *blupcmd[]    = { "brightnessctl", "set", "5%+", NULL };
static const char *wificmd[]    = { "nmcli", "radio", "wifi", "toggle", NULL };
static const char *btcmd[] = { "sh", "-c", "c=$(bluetoothctl devices Connected | awk '{print $2}'); if [ -n \"$c\" ]; then for d in $c; do bluetoothctl disconnect $d; done; else for d in $(bluetoothctl devices Paired | awk '{print $2}'); do bluetoothctl connect $d; done; fi", NULL };

static const Key keys[] = {
	{ MODKEY,                             XKB_KEY_q,         killclient,       {0} },
	{ MODKEY,                             XKB_KEY_j,         setmfact,         {.f = -0.01f} },
	{ MODKEY,                             XKB_KEY_k,         setmfact,         {.f = +0.01f} },
	{ MODKEY,                             XKB_KEY_u,         zoom,             {.i = +1} },
	{ MODKEY,                             XKB_KEY_d,         spawn,            {.v = menucmd} },
	{ MODKEY,                             XKB_KEY_e,         spawn,            {.v = fmcmd} },
	{ MODKEY,                             XKB_KEY_b,         spawn,            {.v = bravecmd} },
	{ 0,                                  XKB_KEY_F1,        spawn,            {.v = mutecmd} },
	{ 0,                                  XKB_KEY_F2,        spawn,            {.v = voldwcmd} },
	{ 0,                                  XKB_KEY_F3,        spawn,            {.v = volupcmd} },
	{ 0,                                  XKB_KEY_F5,        spawn,            {.v = bldwcmd} },
	{ 0,                                  XKB_KEY_F6,        spawn,            {.v = blupcmd} },
	{ 0,                                  XKB_KEY_F8,        spawn,            {.v = wificmd} },
	{ 0,                                  XKB_KEY_F10,       spawn,            {.v = btcmd} },
	{ 0,                                  XKB_KEY_Print,     spawn,            {.v = grimcmd} },
	{ MODKEY,                             XKB_KEY_Print,     spawn,            {.v = reccmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT,          XKB_KEY_Print,     spawn,            {.v = killreccmd} },
	{ MODKEY,                             XKB_KEY_Super_L,   spawn,            {.v = termcmd} },
	{ MODKEY|WLR_MODIFIER_ALT,            XKB_KEY_Delete,    quit,             {0} },
        TAGKEYS(XKB_KEY_1, 0),
	TAGKEYS(XKB_KEY_2, 1),
	TAGKEYS(XKB_KEY_3, 2),
};

static const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};

static const Axis axes[] = {
	{ 0, 0, NULL, {0} },
};
