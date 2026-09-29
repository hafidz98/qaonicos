/*
 * main.c - Qabot: display server persisten QaonicOS (App A2).
 *
 * face = pemilik display + framebuffer + input routing + power state
 * machine (RENCANA-app §2.1, §11).  Berjalan selamanya di scheduler
 * kooperatif; TIDAK exit.
 *
 * Protokol token (syscall 62-68):
 *  - EV_MENU (tombol M) saat face pegang token -> SYS_DISPLAY_GRANT
 *    ke uiapp; face berhenti render selama uiapp pegang token.
 *  - uiapp mengembalikan via SYS_DISPLAY_RELEASE -> face tampil lagi.
 *
 * Power (§11): idle 120 dtk di Qabot -> display off (flush hitam sekali,
 * lalu berhenti render); 300 dtk -> sleep (tetap off, v1); input apa
 * pun -> wake.  uiapp bisa minta sleep via SYS_DISPLAY_SLEEP.
 *
 * Render: strip 240x24 RGB565 (11.5 KB, .bss), pacing via SYS_UPTIME
 * (33 ms ~ 30 fps).  Tiap iterasi loop diakhiri SYS_YIELD agar uiapp
 * + net kebagian CPU.
 */
#include "ulib/ulib.h"
#include "face/face.h"

#define STRIP_H		24
#define NSTRIP		(FACE_H / STRIP_H)	/* 10 */
#define FRAME_MS	33			/* target ~30 fps */

/* Power timeouts, ms (RENCANA-app §11; configurable, bukan magic). */
#define UI_IDLE_DISPLAY_OFF_S	120u
#define UI_SLEEP_AFTER_S	300u

/* Strip buffer di .bss (11.5 KB). */
static uint16_t	strip[FACE_W * STRIP_H];

/* Flush satu frame penuh dari renderer saat ini. */
static void
flush_frame(void)
{
	int s, rc;
	for (s = 0; s < NSTRIP; s++) {
		face_render_strip(strip, s * STRIP_H, STRIP_H);
		rc = sys_display_flush(0, (unsigned)(s * STRIP_H),
				       FACE_W, STRIP_H,
				       strip, sizeof(strip));
		if (rc != 0) {
			puts("face FAIL: SYS_DISPLAY_FLUSH\n");
			sys_exit(1);
		}
	}
}

/* Flush layar hitam (display off visual). */
static void
flush_black(void)
{
	int s, i, rc;
	for (i = 0; i < FACE_W * STRIP_H; i++)
		strip[i] = 0x0000;
	for (s = 0; s < NSTRIP; s++) {
		rc = sys_display_flush(0, (unsigned)(s * STRIP_H),
				       FACE_W, STRIP_H,
				       strip, sizeof(strip));
		if (rc != 0) {
			puts("face FAIL: flush_black\n");
			sys_exit(1);
		}
	}
}

__attribute__((section(".text.start")))
void
_start(void)
{
	struct qaon_display di;
	unsigned last_frame, now, last_input, idle_ms;
	int ev, st;
	int display_on = 1;

	puts("face: Qabot server starting (USR mode)\n");

	if (sys_display_info(&di, sizeof(di)) != 0) {
		puts("face FAIL: SYS_DISPLAY_INFO\n");
		sys_exit(1);
	}
	if (di.width != FACE_W || di.height != FACE_H) {
		puts("face FAIL: geometri display tak dikenal\n");
		sys_exit(1);
	}
	puts("face: display 240x240 ok\n");

	face_init();
	face_set_expr(EXPR_IDLE);
	last_frame = sys_uptime();
	last_input = last_frame;

	for (;;) {
		/* uiapp pegang token: jangan render, jangan baca event. */
		st = sys_display_status();
		if (st != 0) {
			sys_yield();
			continue;
		}

		/* Permintaan sleep dari uiapp (Power -> Sleep Now). */
		if (sys_display_sleep(0) != 0 && display_on) {
			puts("face: sleep requested -> display off\n");
			flush_black();
			display_on = 0;
			idle_ms = (unsigned)(UI_IDLE_DISPLAY_OFF_S * 1000u);
		}

		/* Input: hanya face yang routing (pemegang token). */
		now = sys_uptime();
		ev = sys_display_get_event();
		if (ev != EV_NONE) {
			last_input = now;
			if (!display_on) {
				display_on = 1;	/* wake: input apa pun */
				puts("face: wake\n");
			}
			if (ev == EV_MENU && display_on)
				sys_display_grant();	/* -> uiapp */
		}
		idle_ms = now - last_input;

		/* Power state machine (§11). */
		if (display_on &&
		    idle_ms >= (unsigned)(UI_IDLE_DISPLAY_OFF_S * 1000u)) {
			puts("face: idle -> display off\n");
			flush_black();
			display_on = 0;
		}
		/* UI_SLEEP_AFTER_S: v1 = tetap display off (CPU idle via
		 * yield); sleep beneran (clock gating) diverifikasi di HW. */

		/* Render Qabot bila display on. */
		if (display_on && (now - last_frame) >= FRAME_MS) {
			last_frame = now;
			face_update(FRAME_MS);
			flush_frame();
		}

		sys_yield();
	}
}
