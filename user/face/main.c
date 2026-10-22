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

#define FACE_SETUP	1	/* ONB-2 states */
#define FACE_FINALE	2
#define FACE_NORMAL	0

/* Local syscall wrappers (98 bootstate / 94+96+97 littlefs),
 * same pattern as sh.c's -- ulib does not wrap these yet. */
static int
face_bs(int sub, unsigned *buf)
{
	register int r7 __asm__("r7") = 98;
	register int r0 __asm__("r0") = sub;
	register unsigned *r1 __asm__("r1") = buf;
	__asm__ volatile("svc #0" : "+r"(r0)
	    : "r"(r7), "r"(r1) : "memory");
	return r0;
}

static int
face_lfs(int num, const char *path, void *buf, int len)
{
	register int r7 __asm__("r7") = num;
	register const char *r0 __asm__("r0") = path;
	register void *r1 __asm__("r1") = buf;
	register int r2 __asm__("r2") = len;
	__asm__ volatile("svc #0" : "+r"(r0)
	    : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return (int)r0;
}

/* Build the ONB-2 setup payload "QAON1:<secret>:QaonicOS-Setup".
 * The secret is a 32-hex per-device value kept in cfg-secret
 * (created on first need). In QEMU littlefs is RAM-backed, so
 * it is per-boot there; on hardware it persists. The production
 * session secret is minted by the C6 companion (ONB-3) -- this
 * one only has to be stable for one setup session. */
static void
build_payload(char *out)
{
	static const char hx[] = "0123456789abcdef";
	char sec[33];
	int n = face_lfs(96, "cfg-secret", sec, 32), i;
	unsigned s;
	unsigned info[4];

	if (n != 32) {
		face_bs(0, info);
		s = sys_uptime() ^ (info[0] << 16) ^ 0x9E3779B9u;
		if (s == 0)
			s = 0x51ED5EEDu;
		for (i = 0; i < 32; i++) {
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			sec[i] = hx[s & 15u];
		}
		face_lfs(97, "cfg-secret", sec, 32);
	}
	sec[32] = 0;
	{
		const char *pre = "QAON1:", *post = ":QaonicOS-Setup";
		int k = 0;
		while (*pre)
			out[k++] = *pre++;
		for (i = 0; i < 32; i++)
			out[k++] = sec[i];
		while (*post)
			out[k++] = *post++;
		out[k] = 0;
	}
}

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
	unsigned last_poll;
	int fstate = FACE_NORMAL;
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
	last_poll = last_frame;

	/* ONB-2: unprovisioned -> setup screen until the flag flips. */
	{
		char payload[64];

		if (face_bs(3, (unsigned *)0) & 1) {
			face_set_hint(1);
		} else {
			face_lfs(94, (const char *)0, (void *)0, 0);
			build_payload(payload);
			face_set_setup(payload);
			fstate = FACE_SETUP;
			puts("face: setup mode, payload ");
			puts(payload);
			puts("\n");
		}
	}

	for (;;) {
		/* ONB-2: poll the provisioned flag ~1 Hz. */
		now = sys_uptime();
		if (fstate != FACE_NORMAL &&
		    (unsigned)(now - last_poll) >= 1000u) {
			last_poll = now;
			if (fstate == FACE_SETUP &&
			    (face_bs(3, (unsigned *)0) & 1)) {
				face_set_setup((const char *)0);
				face_set_expr(EXPR_IDLE);
				face_set_status_text(
				    "TEKAN TOMBOL UNTUK MENU");
				face_set_hint(1);
				fstate = FACE_FINALE;
				puts("face: provisioned -> finale\n");
			}
		}
		/* uiapp pegang token: jangan render, jangan baca event. */
		st = sys_display_status();
		if (st != 0) {
			sys_yield();
			continue;
		}

		/* Q4: permintaan ekspresi dari harness (qabotd) atau program
		 * lain. Poll tiap iterasi; murah (satu SVC). */
		{
			unsigned req_expr;
			char req_text[24];
			if (sys_face_poll(&req_expr, req_text) == 0 &&
			    req_expr < 8u) {
				face_set_expr((face_expr_t)req_expr);
				face_set_status_text(req_text);
			}
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
			if (fstate == FACE_FINALE && ev == EV_MENU) {
				/* D-GUI-3: first menu press ends the
				 * hands-on finale (back to plain idle). */
				face_set_expr(EXPR_IDLE);
				fstate = FACE_NORMAL;
			}
			if (ev == EV_MENU && display_on)
				sys_display_grant();	/* -> uiapp */
		}
		idle_ms = now - last_input;

		/* Power state machine (§11). The setup screen never
		 * blanks: setup is an active task (ONB-2). */
		if (display_on && fstate != FACE_SETUP &&
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
