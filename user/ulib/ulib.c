/*
 * ulib.c - Implementasi wrapper syscall QaonicOS (Fase B).
 *
 * ABI: nomor di r7, argumen di r0-r5, return di r0; svc #0.
 * "lr" masuk clobber karena svc menimpanya (pelajaran Clang P6).
 */
#include "ulib.h"

int
sys_write(int fd, const void *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #20\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (fd), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_yield(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #21\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

void
sys_exit(int code)
{
	__asm__ volatile(
		"mov r0, %0\n\t"
		"mov r7, #22\n\t"
		"svc #0"
		:
		: "r" (code)
		: "r0", "r7", "lr", "memory", "cc");
	for (;;) { }	/* tak tercapai */
}

void *
sys_sbrk(int incr)
{
	void *ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r7, #24\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (incr)
		: "r0", "r7", "lr", "memory", "cc");
	return ret;
}

unsigned
ustrlen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

int
puts(const char *s)
{
	return sys_write(1, s, ustrlen(s));
}
