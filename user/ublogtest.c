/*
 * user/ublogtest.c -- B2 diagnostic: SYS_BOOTLOG from one-shot ctx.
 *
 * Prints UBLOGTEST count=<n> get0=<rc> then PASS/FAIL.
 */
#include "ulib/ulib.h"

static int
sys_bootlog(unsigned idx, char *rec)
{
	register unsigned _r0 __asm__("r0") = idx;
	register unsigned _r1 __asm__("r1") = (unsigned)rec;
	int ret;
	__asm__ volatile(
		"mov r7, #99\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static void
put_u(unsigned v)
{
	char buf[12];
	int i = 0;

	if (v == 0) {
		u_put("0");
		return;
	}
	while (v && i < 11) {
		buf[i++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	while (i > 0) {
		char c[2];
		c[0] = buf[--i];
		c[1] = 0;
		u_put(c);
	}
}

static char rec[64];

__attribute__((section(".text.start")))
void
_start(void)
{
	int cnt, rc;

	u_put("UBLOGTEST: count...");
	cnt = sys_bootlog(0xffffffffu, rec);
	u_put("=");
	put_u((unsigned)cnt);
	u_put(" getall...");
	{
		int i;
		rc = 0;
		for (i = 0; i < cnt; i++) {
			u_put("[");
			put_u((unsigned)i);
			u_put("]");
			if (sys_bootlog((unsigned)i, rec) != 0) {
				rc = -1;
				break;
			}
		}
	}
	u_put("get0...");
	u_put("rc=");
	put_u((unsigned)rc);
	u_put("\n");
	if (cnt > 0 && rc == 0)
		u_put("UBLOGTEST: PASS\n");
	else
		u_put("UBLOGTEST: FAIL\n");
	sys_exit(0);
}
