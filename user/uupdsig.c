/*
 * user/uupdsig.c -- B5.3: signed update package verification test.
 *
 * Packages are generated on the host by scripts/sign-update.py
 * (dev test key) into uupdsig_old.h (version 1) and
 * uupdsig_new.h (version 250). Cases, in order (negatives first
 * so state stays untouched):
 *   1. tampered payload byte  -> reject -3
 *   2. tampered version field -> reject -3
 *   3. bad magic              -> reject -3
 *   4. valid sig, old version -> reject -2 (anti-rollback)
 *   5. valid package v250     -> staged (0); info shows pending
 *   6. cancel staged (sub 10) -> 0; info shows idle
 * Prints UUPDSIG: PASS/FAIL.
 */
#include "ulib/ulib.h"

#include "uupdsig_old.h"
#include "uupdsig_new.h"

static int
sys_bs_buf(unsigned sub, const void *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = sub;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = len;
	int ret;
	__asm__ volatile(
		"mov r7, #98\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static int
sys_bs_info(unsigned info[4])
{
	return sys_bs_buf(6u, info, 0u);
}

static void
copy_pkg(unsigned char *dst, const unsigned char *src, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

__attribute__((section(".text.start")))
void
_start(void)
{
	static unsigned char buf[512];
	unsigned info[4];
	int fails = 0;

	/* 1: tampered payload (last byte). */
	copy_pkg(buf, pkg_new, pkg_new_len);
	buf[pkg_new_len - 1u] ^= 0x01u;
	if (sys_bs_buf(9u, buf, pkg_new_len) != -3)
		fails++;

	/* 2: tampered version field (offset 4). */
	copy_pkg(buf, pkg_new, pkg_new_len);
	buf[4] ^= 0x01u;
	if (sys_bs_buf(9u, buf, pkg_new_len) != -3)
		fails++;

	/* 3: bad magic. */
	copy_pkg(buf, pkg_new, pkg_new_len);
	buf[0] = 'X';
	if (sys_bs_buf(9u, buf, pkg_new_len) != -3)
		fails++;

	/* 4: valid signature but version 1 <= current: downgrade.
	 * When a real update flow owns the state (staged/trial),
	 * staging is refused as busy instead -- both prove the
	 * package was verified then refused. */
	{
		int busy = 0;
		int r;

		if (sys_bs_info(info) == 0 && info[2] != 0u)
			busy = 1;
		r = sys_bs_buf(9u, pkg_old, pkg_old_len);
		if (busy ? (r != -1) : (r != -2))
			fails++;
		/* 5+6 need the idle state; during a trial/staged boot
		 * they are skipped (the negatives above still ran). */
		if (!busy) {
			/* 5: the genuine v250 package stages -- unless
			 * the device already runs >= v250 (possible
			 * when an earlier test boot was power-cut
			 * after staging and the trial then confirmed
			 * legitimately). Then -2 is the correct
			 * answer and there is nothing to cancel. */
			r = sys_bs_buf(9u, pkg_new, pkg_new_len);
			if (r == 0) {
				if (sys_bs_info(info) != 0 ||
				    info[2] != 1u || info[3] != 250u)
					fails++;
				/* 6: cancel returns to idle. */
				if (sys_bs_buf(10u, buf, 0u) != 0)
					fails++;
				if (sys_bs_info(info) != 0 ||
				    info[2] != 0u)
					fails++;
			} else if (r == -2) {
				if (sys_bs_info(info) != 0 ||
				    info[0] < 250u)
					fails++;
			} else {
				fails++;
			}
		}
	}

	if (fails == 0)
		u_put("UUPDSIG: PASS\n");
	else
		u_put("UUPDSIG: FAIL\n");
	sys_exit(fails);
}
