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

/* Status-bar icons (UI-8): a real icon font, not
 * hand-drawn masks (UI-7 masks: user feedback
 * 2026-10-27, "masih ga bagus"). The font is
 * Material Symbols Outlined v2.978 (Apache 2.0),
 * subset by lv_font_conv to 5 glyphs at 18 px / 4 bpp
 * (see DEVLOG-UI8 section 3 + README Sources):
 * wifi U+E63E, wifi_off U+E648, bluetooth U+E1A7,
 * bluetooth_disabled U+E1A9, battery_0_bar U+EBDC.
 * The port owns three labels on the top layer (one
 * glyph each, right-aligned, 4 px gaps, dim gray
 * 0x808080); the shell's build_status_bar() calls
 * uilvgl_set_radio_icons() once, which creates the
 * labels (so the UI-1 smoke screen, which never
 * calls it, stays icon-free) and sets wifi/BT text.
 * OFF = the font's real wifi_off /
 * bluetooth_disabled glyphs (no hand-drawn slash).
 * Battery is the outline glyph only, never a level
 * (no ADC) -- the honest-state rule. QEMU defaults
 * are OFF/OFF: no radio exists there. Live state
 * (K5): the shell's radio poll timer calls
 * uilvgl_set_radio_icons() with linkd's live
 * WIFI.STATUS / BLE.STATUS (STATUS-gated, short
 * timeout); the menu-build path itself never RPCs,
 * so menu appearance costs zero RPCs even with
 * linkd dead. Labels die with the display in
 * uilvgl_shell_end(); begin() clears the pointers
 * for the fresh display. */
extern const lv_font_t lv_font_qaon_icons;
#define ICON_GRAY_HEX	0x808080u
/* UTF-8 for the five PUA codepoints. */
#define ICON_WIFI	"\xEE\x98\xBE"	/* U+E63E */
#define ICON_WIFI_OFF	"\xEE\x99\x88"	/* U+E648 */
#define ICON_BT		"\xEE\x86\xA7"	/* U+E1A7 */
#define ICON_BT_OFF	"\xEE\x86\xA9"	/* U+E1A9 */
#define ICON_BATT	"\xEE\xAF\x9C"	/* U+EBDC */
#define ICON_Y		3	/* top offset in the 24 px bar */
#define ICON_GAP	4
#define ICON_MARGIN	6
#define ICON_ADV	18	/* one glyph advance at 18 px */

static lv_obj_t *icon_wifi_label;
static lv_obj_t *icon_bt_label;
static lv_obj_t *icon_batt_label;

static lv_obj_t *
icon_mk_label(int right_offset)
{
	lv_obj_t *l = lv_label_create(lv_layer_top());

	lv_obj_set_style_text_font(l, &lv_font_qaon_icons, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(ICON_GRAY_HEX), 0);
	lv_obj_align(l, LV_ALIGN_TOP_RIGHT, right_offset, ICON_Y);
	return l;
}

void
uilvgl_set_radio_icons(int wifi_on, int bt_on)
{
	/* Batt at the margin, BT one glyph + gap left
	 * of it, wifi one more glyph + gap left of
	 * that. */
	if (!icon_batt_label) {
		icon_batt_label = icon_mk_label(-ICON_MARGIN);
		lv_label_set_text(icon_batt_label, ICON_BATT);
	}
	if (!icon_bt_label)
		icon_bt_label =
		    icon_mk_label(-ICON_MARGIN - ICON_ADV - ICON_GAP);
	if (!icon_wifi_label)
		icon_wifi_label = icon_mk_label(-ICON_MARGIN - 2 * ICON_ADV -
		    2 * ICON_GAP);
	lv_label_set_text(icon_wifi_label,
	    wifi_on ? ICON_WIFI : ICON_WIFI_OFF);
	lv_label_set_text(icon_bt_label, bt_on ? ICON_BT : ICON_BT_OFF);
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
	icon_wifi_label = NULL;
	icon_bt_label = NULL;
	icon_batt_label = NULL;
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
	/* Status-bar icons are LVGL labels (UI-8), not flush
	 * painting, so flush just forwards. */
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
