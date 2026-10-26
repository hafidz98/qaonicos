/*
 * lv_port.c - LVGL v9.5.0 port + smoke screen (UI-1).
 *
 * Display: one 240x24 RGB565 partial buffer (11,520 B,
 * exactly today's strip size); flush_cb forwards each
 * invalidated rectangle to SYS_DISPLAY_FLUSH, which
 * already takes arbitrary rectangles. Tick: sys_uptime()
 * deltas fed to lv_tick_inc(). Input: keypad indev that
 * drains the EV queue (EV_* -> LV_KEY_*); BACK ends the
 * smoke run. No kernel change, no face change.
 *
 * A failed flush is counted and printed, never fatal:
 * the C6L-5 run saw one transient face-side
 * SYS_DISPLAY_FLUSH failure, and UI-1 tracks whether
 * it reproduces on the uiapp path instead of hiding
 * it behind a daemon exit.
 */
#include "ulib/ulib.h"
#include "lv_port.h"
#include "lvgl/lvgl.h"

#define UI_W		240
#define UI_H		240
#define STRIP_H		24
#define IDLE_LIMIT_MS	30000u		/* mirrors UI_IDLE_RETURN_S */

/* lfs syscalls are not exposed by ulib (same file-local
 * trap wrappers as user/ulink5d.c): 94 = MOUNT,
 * 96 = READ, 97 = WRITE. Used for the smoke marker
 * (UI-1) and the Settings screen (UI-3). */
static int
lv_lfs_simple(unsigned num)
{
	int ret;
	__asm__ volatile(
		"mov r7, %1\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (num)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
uilvgl_lfs_read(const char *path, char *buf, unsigned max)
{
	register unsigned _r0 __asm__("r0") = (unsigned)path;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = max;
	int ret;
	__asm__ volatile(
		"mov r7, #96\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* Timezone offset cache (UI-4): fmt_clock in lv_shell.c
 * and ui.c both call uilvgl_tz_offset_min() on every
 * clock render, so the cfg-tz read is cached. The
 * cache is invalidated on any cfg-tz write through
 * this file (the Settings screen's path) and expires
 * after 5 s otherwise (shell `setup tz` writes around
 * this cache, from another program). */
static int	tz_cached = 0;
static int	tz_cache_min = 420;	/* WIB fallback */
static unsigned	tz_cache_ms;

static int
tz_parse(const char *s)
{
	/* Strict "+HHMM"/"-HHMM" (the format setup and
	 * the Settings screen write). Anything else is
	 * malformed and falls back to WIB. */
	int sign = 1, hh, mm, i;
	if (s[0] == '+')
		i = 1;
	else if (s[0] == '-')
		i = 1, sign = -1;
	else
		return 420;
	while (i < 5) {
		if (s[i] < '0' || s[i] > '9')
			return 420;
		i++;
	}
	if (s[5] != 0)
		return 420;
	hh = (s[1] - '0') * 10 + (s[2] - '0');
	mm = (s[3] - '0') * 10 + (s[4] - '0');
	if (hh > 14 || mm > 59)
		return 420;
	return sign * (hh * 60 + mm);
}

int
uilvgl_tz_offset_min(void)
{
	char buf[16];
	int n;
	unsigned now = sys_uptime();

	if (tz_cached && now - tz_cache_ms < 5000u)
		return tz_cache_min;
	n = uilvgl_lfs_read("cfg-tz", buf, sizeof(buf) - 1u);
	if (n <= 0)
		tz_cache_min = 420;
	else {
		buf[n] = 0;
		tz_cache_min = tz_parse(buf);
	}
	tz_cached = 1;
	tz_cache_ms = now;
	return tz_cache_min;
}

int
uilvgl_lfs_write(const char *path, const char *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = (unsigned)path;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = len;
	int ret, i, same = 1;
	const char *want = "cfg-tz";
	__asm__ volatile(
		"mov r7, #97\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	/* A cfg-tz write invalidates the offset cache so
	 * the clocks move on the next render, not after
	 * the 5 s TTL. Compare without strlen (no libc). */
	for (i = 0; i < 6; i++) {
		if (path[i] != want[i]) {
			same = 0;
			break;
		}
	}
	if (same)
		tz_cached = 0;
	return ret;
}

/* Byte buffer, exactly one 240x24 RGB565 strip
 * (11,520 B). NOTE: lv_color_t in LVGL v9 is a 3-byte
 * RGB888 struct regardless of LV_COLOR_DEPTH, so a
 * lv_color_t array would oversize this by 5,760 B;
 * the renderer writes RGB565 pixels (2 B/px) into
 * this byte buffer, and flush_cb forwards w*h*2. */
static uint8_t draw_buf[UI_W * STRIP_H * 2];
static lv_display_t *disp;
static lv_group_t *grp;
static lv_indev_t *indev;
static lv_obj_t *status_label;
static int exit_requested;
static int raw_keys;
static unsigned last_input_ms;
static unsigned flush_fail_count;
static int lv_inited;

/* Status-bar icons (UI-7), painted into the flush
 * strip. The vendored LVGL subset compiles button +
 * label widgets only, and the Montserrat 14 font
 * carries no glyphs for LV_SYMBOL_WIFI / BLUETOOTH /
 * BATTERY (verified in the font's cmap: 0xF1EB,
 * 0xF293, 0xF244 all absent), so built-in symbols
 * cannot render. Instead the port composites 1-bit
 * icon masks into every flushed area overlapping
 * the top bar, reusing the old hand-rolled strip's
 * glyph shapes (ui.c icon_wifi / icon_bt / battery
 * outline) for parity between old and LVGL screens.
 * The shapes follow the canonical wifi-arcs /
 * bluetooth-rune / battery-outline forms (cf.
 * Lucide wifi, bluetooth -- ISC). Cost: point
 * tables only, no font, no image, no new widget.
 *
 * Geometry (240x24 bar, right-aligned, 6 px margin):
 * wifi 15x11 at x 174, y 6; bluetooth 8x11 at x 194,
 * y 6; battery 20x8 outline + nub at x 210, y 8.
 * Icon gray 0x808080 (RGB565 0x8410) matches the
 * dim text the icons replace. OFF = glyph + diagonal
 * slash. Battery is outline-only, never a fake level
 * (no ADC) -- the honest-state rule. QEMU defaults
 * are OFF/OFF: no radio exists there. Live state
 * (uproto WIFI.STATUS / BLE.STATUS, the same calls
 * the wifi/ble screens already make) is a follow-up,
 * not wired here: a blocking RPC in the menu-build
 * path would stall menu appearance on linkd trouble.
 * Painting is enabled only once the shell builds its
 * status bar (uilvgl_set_radio_icons), so the UI-1
 * smoke screen stays icon-free. */
#define ICON_GRAY	0x8410u	/* 0x808080 in RGB565 */
#define ICON_WIFI_X	174
#define ICON_WIFI_Y	6
#define ICON_BT_X	194
#define ICON_BT_Y	6
#define ICON_BATT_X	210
#define ICON_BATT_Y	8

static int bar_icons;
static int radio_wifi_on;
static int radio_bt_on;

void
uilvgl_set_radio_icons(int wifi_on, int bt_on)
{
	bar_icons = 1;
	radio_wifi_on = wifi_on ? 1 : 0;
	radio_bt_on = bt_on ? 1 : 0;
}

static void
icon_px(uint8_t *px_map, unsigned w, unsigned h,
	const lv_area_t *area, int gx, int gy)
{
	int ox = gx - area->x1, oy = gy - area->y1;
	uint16_t *px;

	if (ox < 0 || oy < 0 || (unsigned)ox >= w || (unsigned)oy >= h)
		return;
	px = (uint16_t *)px_map + (unsigned)oy * w + (unsigned)ox;
	px[0] = ICON_GRAY;
}

static void
paint_status_icons(uint8_t *px_map, unsigned w, unsigned h,
	const lv_area_t *area)
{
	/* Wifi arcs + dot (box 15x11, dot centre (7,9)
	 * relative): inner, mid, outer arcs, then dot. */
	static const signed char wifi_pts[] = {
		6,8, 7,8, 6,9, 7,9,
		7,6, 5,7, 9,7, 4,8, 10,8,
		7,4, 3,5, 11,5, 2,7, 12,7,
		7,2, 2,4, 12,4, 0,7, 14,7
	};
	/* Bluetooth rune (box 8x11): spine is drawn
	 * separately; these are the triangle edges. */
	static const signed char bt_pts[] = {
		4,0, 5,1, 6,1, 7,2, 6,3, 5,3, 4,4,
		4,6, 5,6, 6,7, 7,8, 6,9, 5,9, 4,10
	};
	int i, j;

	if (!bar_icons)
		return;
	for (i = 0; i < 19; i++)
		icon_px(px_map, w, h, area,
			ICON_WIFI_X + wifi_pts[i * 2],
			ICON_WIFI_Y + wifi_pts[i * 2 + 1]);
	if (!radio_wifi_on)
		for (i = 0; i < 15; i++)
			icon_px(px_map, w, h, area, ICON_WIFI_X + i,
				ICON_WIFI_Y + (i * 10) / 14);
	for (i = 0; i <= 10; i++)
		icon_px(px_map, w, h, area, ICON_BT_X + 3,
			ICON_BT_Y + i);
	for (i = 0; i < 14; i++)
		icon_px(px_map, w, h, area,
			ICON_BT_X + bt_pts[i * 2],
			ICON_BT_Y + bt_pts[i * 2 + 1]);
	if (!radio_bt_on)
		for (i = 0; i < 8; i++)
			icon_px(px_map, w, h, area, ICON_BT_X + i,
				ICON_BT_Y + (i * 10) / 7);
	/* Battery outline + nub, empty inside. */
	for (i = 0; i < 20; i++) {
		icon_px(px_map, w, h, area, ICON_BATT_X + i, ICON_BATT_Y);
		icon_px(px_map, w, h, area, ICON_BATT_X + i,
			ICON_BATT_Y + 7);
	}
	for (i = 0; i < 8; i++) {
		icon_px(px_map, w, h, area, ICON_BATT_X, ICON_BATT_Y + i);
		icon_px(px_map, w, h, area, ICON_BATT_X + 19,
			ICON_BATT_Y + i);
	}
	for (i = 0; i < 2; i++)
		for (j = 0; j < 4; j++)
			icon_px(px_map, w, h, area, ICON_BATT_X + 22 + i,
				ICON_BATT_Y + 2 + j);
}

/* Callbacks, defined below begin(). */
static void flush_cb(lv_display_t *d, const lv_area_t *area,
		     uint8_t *px_map);
static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

/* Shared port lifecycle (UI-2): one display + keypad
 * indev + focus group per run, created by begin() and
 * torn down by end(). Smoke and shell both use it, so
 * neither can leak an indev across token grants. */
lv_group_t *
uilvgl_shell_begin(void)
{
	if (!lv_inited) {
		lv_init();
		lv_inited = 1;
	}
	disp = lv_display_create(UI_W, UI_H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, flush_cb);

	indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(indev, indev_read_cb);

	grp = lv_group_create();
	lv_indev_set_group(indev, grp);

	exit_requested = 0;
	raw_keys = 0;
	last_input_ms = sys_uptime();
	flush_fail_count = 0;
	bar_icons = 0;
	return grp;
}

void
uilvgl_shell_end(void)
{
	/* Indev first: it holds the group pointer, so
	 * deleting it first leaves no dangling link. */
	if (indev) {
		lv_indev_delete(indev);
		indev = NULL;
	}
	if (grp) {
		lv_group_delete(grp);
		grp = NULL;
	}
	if (disp) {
		lv_display_delete(disp);
		disp = NULL;
	}
}

int
uilvgl_take_back(void)
{
	if (!exit_requested)
		return 0;
	exit_requested = 0;
	return 1;
}

unsigned
uilvgl_last_input_ms(void)
{
	return last_input_ms;
}

/* Raw-key mode (UI-3, for the PIN digit-picker): while
 * on, the keypad indev stops draining the EV queue and
 * uilvgl_take_key() hands raw EV codes to the caller
 * instead. BACK in raw mode is a plain key for the
 * picker to consume as cancel -- it deliberately does
 * NOT set exit_requested, so the shell loop's BACK
 * branch cannot double-handle it. Leaving raw mode
 * is always paired with a view rebuild, so no stale
 * key can leak into the next view. */
void
uilvgl_raw_keys(int on)
{
	raw_keys = on ? 1 : 0;
}

int
uilvgl_take_key(void)
{
	int ev = sys_display_get_event();

	switch (ev) {
	case EV_UP:
	case EV_DOWN:
	case EV_LEFT:
	case EV_RIGHT:
	case EV_OK:
	case EV_BACK:
		last_input_ms = sys_uptime();
		return ev;
	default:
		break;	/* EV_NONE, EV_TICK, EV_MENU: no key */
	}
	return EV_NONE;
}

unsigned
uilvgl_flush_failures(void)
{
	return flush_fail_count;
}

static void
flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
	unsigned w = (unsigned)(area->x2 - area->x1 + 1);
	unsigned h = (unsigned)(area->y2 - area->y1 + 1);
	/* Composite the status-bar icons into any flush
	 * overlapping the top bar (clipped inside). */
	if (area->y1 < STRIP_H)
		paint_status_icons(px_map, w, h, area);
	if (sys_display_flush((unsigned)area->x1, (unsigned)area->y1,
			      w, h, px_map, w * h * 2u) != 0) {
		flush_fail_count++;
		puts("UILV: flush FAIL\n");
	}
	lv_display_flush_ready(d);
}

static void
indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	static int pressed = 0;
	int ev, key;

	(void)indev;
	/* Raw-key mode: the picker owns the EV queue, so
	 * the indev reports nothing and drains nothing. */
	if (raw_keys) {
		data->state = LV_INDEV_STATE_RELEASED;
		return;
	}
	/* A press is reported once, then a release: the
	 * keypad model needs the pair, and the EV queue
	 * holds at most one event per read. */
	if (pressed) {
		data->state = LV_INDEV_STATE_RELEASED;
		pressed = 0;
		return;
	}
	ev = sys_display_get_event();
	key = -1;
	/* Keypad group navigation in LVGL moves focus
	 * ONLY on NEXT/PREV: arrow keys are delivered to
	 * the focused object as data instead. So the
	 * 7-key pad maps DOWN -> NEXT (focus forward)
	 * and UP -> PREV (focus back); LEFT/RIGHT stay
	 * arrows for widgets that consume them. */
	switch (ev) {
	case EV_UP: key = LV_KEY_PREV; break;
	case EV_DOWN: key = LV_KEY_NEXT; break;
	case EV_LEFT: key = LV_KEY_LEFT; break;
	case EV_RIGHT: key = LV_KEY_RIGHT; break;
	case EV_OK: key = LV_KEY_ENTER; break;
	case EV_BACK:
		key = LV_KEY_ESC;
		exit_requested = 1;
		break;
	default: break;	/* EV_NONE, EV_TICK, EV_MENU: no key */
	}
	if (key >= 0) {
		data->state = LV_INDEV_STATE_PRESSED;
		data->key = (uint32_t)key;
		pressed = 1;
		last_input_ms = sys_uptime();
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
}

static void
focus_cb(lv_event_t *e)
{
	puts("UILV: focus ");
	puts((const char *)lv_event_get_user_data(e));
	puts("\n");
}

static void
press_cb(lv_event_t *e)
{
	(void)e;
	lv_label_set_text(status_label, "ACTIVATED");
	puts("UILV: activated\n");
}

static void
back_cb(lv_event_t *e)
{
	(void)e;
	exit_requested = 1;
	puts("UILV: back button\n");
}

static lv_obj_t *
mk_button(lv_obj_t *scr, int y, const char *text, const char *name,
	  lv_event_cb_t cb)
{
	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_t *lbl;
	lv_obj_set_size(btn, 160, 44);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, text);
	lv_obj_center(lbl);
	lv_group_add_obj(grp, btn);
	lv_obj_add_event_cb(btn, focus_cb, LV_EVENT_FOCUSED, (void *)name);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	return btn;
}

int
uilvgl_smoke_enabled(void)
{
	char marker[8];

	/* Same marker pattern as the C6L-5 test daemons:
	 * mount is idempotent, a failed mount means the
	 * marker cannot exist, so plain "not enabled". */
	if (lv_lfs_simple(94u) != 0)
		return 0;
	if (uilvgl_lfs_read("ui1-test", marker, sizeof(marker)) < 0)
		return 0;
	return marker[0] == '1';
}

void
uilvgl_run(void)
{
	lv_obj_t *scr, *title, *btn_press, *btn_back;
	unsigned last, now;

	grp = uilvgl_shell_begin();

	scr = lv_screen_active();
	title = lv_label_create(scr);
	lv_label_set_text(title, "QAONIC LVGL");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

	btn_press = mk_button(scr, 64, "PRESS ME", "press", press_cb);
	btn_back = mk_button(scr, 124, "BACK", "back", back_cb);
	(void)btn_back;
	lv_group_focus_obj(btn_press);

	status_label = lv_label_create(scr);
	lv_label_set_text(status_label, "READY");
	lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 190);

	last = sys_uptime();
	puts("UILV: start\n");

	while (sys_display_status() == 1 && !exit_requested) {
		now = sys_uptime();
		lv_tick_inc(now - last);
		last = now;
		lv_timer_handler();
		if (now - last_input_ms >= IDLE_LIMIT_MS) {
			puts("UILV: idle exit\n");
			break;
		}
		sys_yield();
	}

	puts("UILV: exit\n");
	if (flush_fail_count != 0) {
		puts("UILV: flush failures: ");
		/* Small decimal print without printf. */
		{
			char nbuf[12];
			unsigned v = flush_fail_count, i = 0, j;
			if (v == 0)
				nbuf[i++] = '0';
			while (v > 0) {
				nbuf[i++] = (char)('0' + v % 10u);
				v /= 10u;
			}
			for (j = 0; j < i / 2u; j++) {
				char t = nbuf[j];
				nbuf[j] = nbuf[i - 1u - j];
				nbuf[i - 1u - j] = t;
			}
			nbuf[i] = 0;
			puts(nbuf);
		}
		puts("\n");
	}
	uilvgl_shell_end();
}
