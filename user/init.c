/*
 * init.c - Program user pertama QaonicOS (Fase C), diluncurkan kernel
 * saat boot.  Bare-metal, tanpa libc; di-link di INIT_CODE_VA
 * (0x100000) via init.ld, di-embed sebagai blob -> init_img.
 *
 * Peran: SETUP.  Menulis /motd.txt + sentinel /.motd_ready yang
 * ditunggu ucat/uls, lalu uji sbrk singkat (regresi Fase B), lalu
 * SYS_EXIT(0).  Kernel meluncurkan program berikutnya (ucat, uls,
 * uecho, umon) secara berurutan dan memverifikasi artefak di akhir.
 */
#include "ulib/ulib.h"

#define MOTD_STR "QaonicOS Mach3: motd dari init\n"

__attribute__((section(".text.start")))
void
_start(void)
{
	void *brk0, *brk1;
	unsigned char *p;
	int i, ok, fd, w;
	unsigned n;

	puts("init: QaonicOS user task starting (USR mode)\n");

	/* Tulis /motd.txt untuk ucat. */
	n = u_strlen(MOTD_STR);
	fd = u_open("/motd.txt", O_CREAT | O_RDWR);
	if (fd < 0) {
		u_put("init FAIL: open /motd.txt\n");
		u_exit();
	}
	w = u_write((unsigned)fd, MOTD_STR, n);
	u_close((unsigned)fd);
	if (w < 0 || (unsigned)w != n) {
		u_put("init FAIL: tulis /motd.txt\n");
		u_exit();
	}
	if (!u_touch("/.motd_ready")) {
		u_put("init FAIL: sentinel /.motd_ready\n");
		u_exit();
	}
	puts("init: /motd.txt + /.motd_ready ok\n");

	/* Uji sbrk singkat (regresi Fase B). */
	brk0 = sys_sbrk(0);
	brk1 = sys_sbrk(4096);
	if (brk0 != (void *)-1 && brk1 == brk0) {
		p = (unsigned char *)brk0;
		ok = 1;
		for (i = 0; i < 4096; i++)
			p[i] = (unsigned char)(i & 0xFF);
		for (i = 0; i < 4096; i++) {
			if (p[i] != (unsigned char)(i & 0xFF)) {
				ok = 0;
				break;
			}
		}
		puts(ok ? "init: sbrk ok\n" : "init: sbrk FAIL (pola rusak)\n");
		if (!ok)
			u_exit();
	} else {
		puts("init: sbrk FAIL (brk tak valid)\n");
		u_exit();
	}

	sys_yield();
	puts("init: done\n");
	sys_exit(0);
	for (;;) { }	/* tak tercapai */
}
