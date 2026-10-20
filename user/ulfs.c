/*
 * user/ulfs.c -- B1: littlefs self-test.
 *
 * One-shot: asks the kernel to run the littlefs test sequence
 * (format/mount, write+remount+verify, simulated power cut,
 * recovery) and prints the result.
 * Prints ULFS: PASS/FAIL.
 */
#include "ulib/ulib.h"

static unsigned
sys_lfs_runtest(void)
{
	unsigned ret;
	__asm__ volatile(
		"mov r7, #93\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : : "r7", "lr", "memory", "cc");
	return ret;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	unsigned mask = sys_lfs_runtest();

	if (mask == 0x1fu)
		u_put("ULFS: PASS\n");
	else
		u_put("ULFS: FAIL\n");
	sys_exit(mask == 0x1fu ? 0 : 1);
}
