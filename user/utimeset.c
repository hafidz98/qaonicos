/*
 * user/utimeset.c -- Regression test for SYS_TIME_SET / SYS_TIME_GET (F2).
 *
 * One-shot program: for each known-good value, sys_time_set(v) then
 * immediately sys_time_get(); the two must match (modulo <1s tick drift).
 * Prints hex set vs get and UTIMESET: PASS/FAIL.
 */
#include "ulib/ulib.h"

static void
puthex(unsigned v)
{
	static const char *h = "0123456789abcdef";
	char b[11];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 8; i++)
		b[2 + i] = h[(v >> (28 - i * 4)) & 0xFu];
	b[10] = '\n';
	sys_write(1, b, 11u);
}

__attribute__((section(".text.start")))
void
_start(void)
{
	static const unsigned vals[3] = { 0x6abf5a00u, 1790683200u, 0x12345678u };
	unsigned i, got;
	int fails = 0;

	u_put("utimeset: TIME_SET/GET regression\n");
	for (i = 0; i < 3u; i++) {
		sys_time_set(vals[i]);
		got = sys_time_get();
		u_put("  set="); puthex(vals[i]);
		u_put("  get="); puthex(got);
		/* Allow <2s forward drift from timer ticks between set/get. */
		if (got < vals[i] || got - vals[i] > 2u) {
			u_put("  MISMATCH\n");
			fails++;
		}
	}
	u_put(fails == 0 ? "UTIMESET: PASS\n" : "UTIMESET: FAIL\n");
	/* Tinggalkan jam pada nilai waras (bukan 1979) untuk program lain. */
	sys_time_set(1790683200u);
	u_exit();
}
