/*
 * init.c - Task user pertama QaonicOS (Fase B), diluncurkan kernel
 * saat boot.  Bare-metal, tanpa libc; di-link di INIT_CODE_VA
 * (0x100000) via init.ld, di-embed sebagai blob -> init_img.
 *
 * Alur: banner -> "menjalankan hello" (pesan yang sama dengan
 * user/hello.c) -> uji sbrk (alokasi 1 halaman, tulis/baca pola)
 * -> yield (tunggu 1 tick) -> SYS_EXIT(0).  Kernel lalu halt bersih.
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	void *brk0, *brk1;
	unsigned char *p;
	int i, ok;

	puts("init: QaonicOS user task starting (USR mode)\n");

	/* "Menjalankan hello": alur yang sama dengan user/hello.c. */
	puts("hello from user mode\n");

	/* Uji sbrk: minta 1 halaman, tulis & baca pola 0..255. */
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
	} else {
		puts("init: sbrk FAIL (brk tak valid)\n");
	}

	/* Yield: serahkan sisa slice, tunggu 1 tick. */
	sys_yield();
	puts("init: yield ok\n");

	puts("init: done\n");
	sys_exit(0);
	for (;;) { }	/* tak tercapai */
}
