/*
 * main.c - Qabot: program user QaonicOS (App A1).
 *
 * Render loop ekspresi idle -> display 240x240 via SYS_DISPLAY_FLUSH
 * (61).  Framebuffer parsial 1/10: strip 240x24 RGB565 = 11520 byte
 * (masih muat di heap 64 KB/task; frame penuh 115200 byte tidak muat).
 *
 * Pacing: SYS_YIELD menunggu 1 tick timer (10 ms); 2 yield per frame
 * ditambah waktu render -> ~30 fps.
 *
 * Setelah FACE_FRAMES frame: tulis /face.out + sentinel /.face_done
 * (pola koordinasi ramfs Fase C), lalu SYS_EXIT(0).  Kernel
 * memverifikasi sentinel seperti program lain.
 */
#include "ulib/ulib.h"
#include "face/face.h"

#define STRIP_H		24
#define NSTRIP		(FACE_H / STRIP_H)	/* 10 */
#define FRAME_MS	33			/* target ~30 fps */
#define FACE_FRAMES	900			/* ~30 detik animasi */

/* Strip buffer di .bss (11.5 KB). */
static uint16_t	strip[FACE_W * STRIP_H];

__attribute__((section(".text.start")))
void
_start(void)
{
	struct qaon_display di;
	int f, s, rc, fd, w;
	static const char okmsg[] = "QABOT OK\n";

	puts("face: Qabot starting (USR mode)\n");

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

	for (f = 0; f < FACE_FRAMES; f++) {
		face_update(FRAME_MS);
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
		sys_yield();
		sys_yield();
	}

	fd = u_open("/face.out", O_CREAT | O_RDWR);
	if (fd < 0) {
		puts("face FAIL: open /face.out\n");
		sys_exit(1);
	}
	w = u_write((unsigned)fd, okmsg, sizeof(okmsg) - 1u);
	u_close((unsigned)fd);
	if (w != (int)(sizeof(okmsg) - 1u)) {
		puts("face FAIL: tulis /face.out\n");
		sys_exit(1);
	}
	if (!u_touch("/.face_done")) {
		puts("face FAIL: sentinel /.face_done\n");
		sys_exit(1);
	}

	puts("face: done\n");
	sys_exit(0);
	for (;;) { }	/* tak tercapai */
}
