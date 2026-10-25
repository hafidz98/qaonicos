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
 * 96 = READ. Only used for the smoke marker. */
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

static int
lv_lfs_read(const char *path, char *buf, unsigned max)
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

/* Byte buffer, exactly one 240x24 RGB565 strip
 * (11,520 B). NOTE: lv_color_t in LVGL v9 is a 3-byte
 * RGB888 struct regardless of LV_COLOR_DEPTH, so a
 * lv_color_t array would oversize this by 5,760 B;
 * the renderer writes RGB565 pixels (2 B/px) into
 * this byte buffer, and flush_cb forwards w*h*2. */
static uint8_t draw_buf[UI_W * STRIP_H * 2];
static lv_display_t *disp;
static lv_group_t *grp;
static lv_obj_t *status_label;
static int exit_requested;
static unsigned last_input_ms;
static unsigned flush_fail_count;
static int lv_inited;

static void
flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
	unsigned w = (unsigned)(area->x2 - area->x1 + 1);
	unsigned h = (unsigned)(area->y2 - area->y1 + 1);
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
	if (lv_lfs_read("ui1-test", marker, sizeof(marker)) < 0)
		return 0;
	return marker[0] == '1';
}

void
uilvgl_run(void)
{
	lv_obj_t *scr, *title, *btn_press, *btn_back;
	lv_indev_t *indev;
	unsigned last, now;

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

	exit_requested = 0;
	flush_fail_count = 0;
	last = sys_uptime();
	last_input_ms = last;
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
	lv_group_delete(grp);
	grp = NULL;
	lv_display_delete(disp);
	disp = NULL;
}
