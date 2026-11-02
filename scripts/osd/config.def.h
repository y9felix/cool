enum {
	CORNER_TOP_LEFT,
	CORNER_TOP_RIGHT,
	CORNER_BOTTOM_LEFT,
	CORNER_BOTTOM_RIGHT
};

static const int hide_time = 1;
static const int corner = CORNER_TOP_RIGHT;
static const int edge_margin = 30;
static const int step_percentage = 5;

static const int popup_width = 324;
static const int popup_height = 38;
static const int border_width = 2;
static const int icon_size = 20;
static const int square_width = 8;
static const int square_height = 12;
static const int square_gap = 3;
static const int padding_x = 14;
static const int spacing = 12;

static const uint32_t color_bg = 0xFF000000;
static const uint32_t color_text = 0xFFFFFFFF;
static const uint32_t color_accent = 0xFF7E56C3;
static const uint32_t color_inactive_accent = 0xFF333333;
static const uint32_t color_disabled = 0xFF666666;

static const char *backlight_path = "/sys/class/backlight/intel_backlight";
