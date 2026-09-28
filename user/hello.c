/*
 * hello.c - Program uji userspace Fase B: syscall minimal.
 *
 * _start -> SYS_WRITE("hello from user mode") -> SYS_EXIT(0).
 * Di-build sebagai cek kompilasi/link ulib (biner tidak di-embed;
 * yang diluncurkan kernel saat boot adalah init).
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	puts("hello from user mode\n");
	sys_exit(0);
	for (;;) { }	/* tak tercapai */
}
