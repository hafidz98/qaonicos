/*
 * main.c - user/uiapp: LVGL shell + old leaf screens (App A2).
 *
 * Client display via protokol token (§2.1): menunggu GRANT dari face
 * (ACQUIRE), lalu RELEASE (BACK di menu root / idle 30 dtk / Sleep
 * Now). Tidak menyentuh hardware display langsung selain FLUSH.
 *
 * UI-2 flow per token grant: the LVGL shell (lv_shell.c) is the
 * menu. A wired entry runs its existing hand-rolled screen
 * under the same grant; popping that screen (BACK with an
 * empty stack) returns to the shell, while release/idle/token
 * loss releases the token. The UI-1 smoke screen stays
 * marker-gated ahead of the shell so UI1-VERIFY keeps working.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"
#include "lv_port.h"
#include "lv_shell.h"

#define FRAME_MS	33		/* ~30 fps */
#define UI_IDLE_RETURN_S	30u	/* auto-return ke Qabot (§11) */

extern const screen_t scr_wifi;
extern const screen_t scr_ble;
extern const screen_t scr_llm;
extern const screen_t scr_passkey;

/* dari ui.c */
void	ui_render_frame(void);

/* Run one hand-rolled screen under the token grant.
 * Returns 0 to go back to the LVGL menu (the screen
 * popped itself off an empty stack), 1 to release the
 * token (release requested, token lost, or idle — the
 * old loop's behaviour, preserved). */
static int
run_old_screen(const screen_t *s)
{
	unsigned last, now, last_input, idle_ms;
	int ev;

	ui_reset(s);
	last = sys_uptime();
	last_input = last;

	while (sys_display_status() == 1 &&
	       !ui_release_requested()) {
		/* Popped itself: the stack is empty, so the
		 * user asked to go back to the LVGL menu. */
		if (ui_depth() == 0)
			return 0;
		now = sys_uptime();
		ev = sys_display_get_event();
		if (ev != EV_NONE) {
			last_input = now;
			if (ui_top())
				ui_top()->on_event(ev);
		}
		idle_ms = now - last_input;

		/* Auto-return: idle -> Qabot (§11). */
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
	return 1;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	struct qaon_display di;

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

		/* UI-1: marker-gated LVGL smoke screen. Without
		 * /data/ui1-test == "1" the shell below runs;
		 * the smoke run returns on BACK, token loss,
		 * or 30 s idle, and the common release path
		 * below still runs. */
		if (uilvgl_smoke_enabled()) {
			uilvgl_run();
			ui_clear_release();
			if (sys_display_release() != 0)
				puts("uiapp: release FAIL\n");
			else
				puts("uiapp: token released\n");
			continue;
		}

		/* UI-2: LVGL shell menu, then old leaf screens
		 * on demand, looping back to the shell until
		 * it (or an old screen) asks for release. */
		for (;;) {
			int act = uilvgl_shell_run();
			const screen_t *tgt = NULL;
			const char *tname = NULL;

			switch (act) {
			case SHELL_WIFI: tgt = &scr_wifi; tname = "wifi"; break;
			case SHELL_BLE: tgt = &scr_ble; tname = "ble"; break;
			case SHELL_LLM: tgt = &scr_llm; tname = "llm"; break;
			case SHELL_PASSKEY: tgt = &scr_passkey; tname = "passkey"; break;
			default: break;	/* SHELL_RELEASE */
			}
			if (tgt == NULL)
				break;
			puts("UILV2: oldscreen ");
			puts(tname);
			puts("\n");
			if (run_old_screen(tgt) != 0)
				break;
			ui_clear_release();
		}

		ui_clear_release();
		if (sys_display_release() != 0)
			puts("uiapp: release FAIL\n");
		else
			puts("uiapp: token released\n");
	}
}
