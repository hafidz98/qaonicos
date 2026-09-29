/*
 * main.c - user/uiapp: menu/settings/monitor/power (App A2).
 *
 * Client display via protokol token (§2.1): menunggu GRANT dari face
 * (ACQUIRE), me-render layar via screen manager + FLUSH, lalu RELEASE
 * (BACK di menu root / idle 30 dtk / Sleep Now).  Tidak menyentuh
 * hardware display langsung selain FLUSH.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

#define FRAME_MS	33		/* ~30 fps */
#define UI_IDLE_RETURN_S	30u	/* auto-return ke Qabot (§11) */

extern const screen_t scr_menu;

/* dari ui.c */
void	ui_render_frame(void);

__attribute__((section(".text.start")))
void
_start(void)
{
	struct qaon_display di;
	unsigned last, now, last_input, idle_ms;
	int ev;

	puts("uiapp: starting (USR mode)\n");

	if (sys_display_info(&di, sizeof(di)) != 0 ||
	    di.width != 240 || di.height != 240) {
		puts("uiapp FAIL: display\n");
		sys_exit(1);
	}

	for (;;) {
		/* Tunggu token dari face. */
		while (sys_display_acquire() == 0)
			sys_yield();
		puts("uiapp: token acquired\n");

		ui_reset(&scr_menu);
		last = sys_uptime();
		last_input = last;

		while (sys_display_status() == 1 &&
		       !ui_release_requested()) {
			now = sys_uptime();
			ev = sys_display_get_event();
			if (ev != EV_NONE) {
				last_input = now;
				if (ui_top())
					ui_top()->on_event(ev);
			}
			idle_ms = now - last_input;

			/* Auto-return: idle di menu -> Qabot (§11). */
			if (idle_ms >= (unsigned)(UI_IDLE_RETURN_S * 1000u)) {
				puts("uiapp: idle -> release\n");
				break;
			}

			if ((now - last) >= FRAME_MS) {
				last = now;
				ui_render_frame();
			}
			sys_yield();
		}

		ui_clear_release();
		if (sys_display_release() != 0)
			puts("uiapp: release FAIL\n");
		else
			puts("uiapp: token released\n");
	}
}
