static const char bat_path[] = "/sys/class/power_supply/BAT0";
static const int poll_interval = 20;
static const int level_low = 15;
static const int level_crit = 5;
static const int default_scale = 1;
static const int default_grab = 1;

static const int popup_width = 440;
static const int popup_height = 176;
static const int border_width = 3;

static const uint32_t color_bg = 0xFF000000;
static const uint32_t color_text = 0xFFFFFFFF;
static const uint32_t color_low_accent = 0xFF7E56C3;
static const uint32_t color_crit_accent = 0xFF8000FF;

static const char *msg_low = "BATTERY LOW";
static const char *msg_critical = "BATTERY CRITICAL";
static const char *msg_action = "PLUG IN CHARGER";
