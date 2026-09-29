/*
 * scr_power.c - Power menu (§11): Sleep Now / Shutdown.
 *
 * Sleep Now: minta face tidur via SYS_DISPLAY_SLEEP(1), lalu lepas
 * token (face memadamkan display).  Shutdown: layar hitam + teks,
 * lalu diam (yield selamanya).  Restart butuh driver reset/PSCI —
 * belum tersedia, jadi tidak ditampilkan (jujur).
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

static const char *items[2] = { "SLEEP NOW", "SHUTDOWN" };
static int	sel = 0;

#define Y0	64
#define LH	30

static void
pwr_enter(void)
{
	sel = 0;
}

/* Layar hitam + teks tengah via strip lokal. */
static void
halt_screen(const char *msg)
{
	static uint16_t buf[240 * 24];
	int s, i;
	for (s = 0; s < 10; s++) {
		ui_set_strip(buf, s * 24, 24);
		ui_clear(0x0000);
		if (s == 4)
			ui_text_center(s * 24 + 8, msg, C_DIM);
		sys_display_flush(0, (unsigned)(s * 24), 240, 24,
				  buf, sizeof(buf));
	}
	(void)i;
}

static void
pwr_event(int ev)
{
	switch (ev) {
	case EV_UP:
		if (sel > 0) sel--;
		break;
	case EV_DOWN:
		if (sel < 1) sel++;
		break;
	case EV_OK:
		if (sel == 0) {
			/* Sleep Now */
			sys_display_sleep(1);
			ui_request_release();
		} else {
			/* Shutdown */
			halt_screen("DIMATIKAN");
			puts("uiapp: shutdown\n");
			for (;;)
				sys_yield();
		}
		break;
	case EV_BACK:
		ui_pop();
		break;
	default:
		break;
	}
}

static void
pwr_render(void)
{
	int i;
	ui_text_center(22, "POWER", C_DIM);
	for (i = 0; i < 2; i++) {
		int y = Y0 + i * LH;
		if (i == sel) {
			ui_rect(40, y - 5, UI_W - 80, 24, C_SEL);
			ui_border(40, y - 5, UI_W - 80, 24, C_ACCENT);
		}
		ui_text_center(y, items[i], i == sel ? C_ACCENT : C_FG);
	}
}

const screen_t scr_power = {
	pwr_enter, 0, pwr_event, pwr_render
};
