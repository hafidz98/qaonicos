/*
 * scr_power.c - Power menu (D-GUI): Shutdown / Restart /
 * Sleep Now / Back.
 *
 * Shutdown and Restart are real since FND-1: SYS_POWEROFF
 * (PSCI SYSTEM_OFF) and SYS_RESET (PSCI SYSTEM_RESET). The
 * kernel records bootstate reason 4 (software request)
 * before acting. On success the syscalls never return; if
 * one ever does (platform refused), the screen falls back
 * to the old behaviour: message shown, yield forever.
 *
 * Sleep Now: ask the face to sleep via SYS_DISPLAY_SLEEP(1),
 * then release the token (the face blanks the display).
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

static const char *items[4] = {
	"SHUTDOWN", "RESTART", "SLEEP NOW", "BACK"
};
static int	sel = 0;

#define Y0	58
#define LH	30

static void
pwr_enter(void)
{
	sel = 0;
}

/* Black screen + centred text, flushed strip by strip. */
static void
halt_screen(const char *msg)
{
	static uint16_t buf[240 * 24];
	int s;
	for (s = 0; s < 10; s++) {
		ui_set_strip(buf, s * 24, 24);
		ui_clear(0x0000);
		if (s == 4)
			ui_text_center(s * 24 + 8, msg, C_DIM);
		sys_display_flush(0, (unsigned)(s * 24), 240, 24,
				  buf, sizeof(buf));
	}
}

/* Only reached if the platform refused the power syscall. */
static void
halt_forever(void)
{
	for (;;)
		sys_yield();
}

static void
pwr_event(int ev)
{
	switch (ev) {
	case EV_UP:
		if (sel > 0) sel--;
		break;
	case EV_DOWN:
		if (sel < 3) sel++;
		break;
	case EV_OK:
		if (sel == 0) {
			/* Shutdown (FND-1: real poweroff) */
			halt_screen("SHUTTING DOWN");
			puts("uiapp: shutdown (SYS_POWEROFF)\n");
			(void)sys_poweroff();
			halt_forever();
		} else if (sel == 1) {
			/* Restart (FND-1: real reset) */
			halt_screen("RESTARTING");
			puts("uiapp: restart (SYS_RESET)\n");
			(void)sys_reset();
			halt_forever();
		} else if (sel == 2) {
			/* Sleep Now: existing display-sleep path */
			sys_display_sleep(1);
			ui_request_release();
		} else {
			/* Back */
			ui_pop();
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
	for (i = 0; i < 4; i++) {
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
