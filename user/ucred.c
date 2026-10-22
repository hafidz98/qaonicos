/*
 * user/ucred.c -- B5.5: credential store test (syscall 104).
 *
 * Cases:
 *   0. kernel self-test (AES-256-GCM host vectors + store
 *      roundtrip) -> bitmask must be 0xF
 *   1. set/get roundtrip via SYS_CRED
 *   2. raw file (SYS_LFS_READ of cfg-testcred) starts with
 *      "QSEC" and does NOT contain the plaintext
 *   3. one flipped ciphertext byte -> get fails (-1)
 *   4. legacy plaintext cfg-apikey migrates: get returns the
 *      plaintext and the raw file becomes QSEC
 *   5. clear -> present 0
 * The pre-existing cfg-apikey bytes are snapshotted and
 * restored (the one-shot state-tolerance rule).
 * Prints UCREDSTORE: PASS/FAIL.
 */
#include "ulib/ulib.h"

static int
svc3(unsigned num, unsigned a0, unsigned a1, unsigned a2)
{
	register unsigned _r0 __asm__("r0") = a0;
	register unsigned _r1 __asm__("r1") = a1;
	register unsigned _r2 __asm__("r2") = a2;
	register unsigned _r7 __asm__("r7") = num;
	int ret;
	__asm__ volatile(
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1), "r" (_r2), "r" (_r7)
		: "lr", "memory", "cc");
	return ret;
}

static int
cred4(unsigned sub, const char *name, void *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = sub;
	register unsigned _r1 __asm__("r1") = (unsigned)name;
	register unsigned _r2 __asm__("r2") = (unsigned)buf;
	register unsigned _r3 __asm__("r3") = len;
	register unsigned _r7 __asm__("r7") = 104u;
	int ret;
	__asm__ volatile(
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1), "r" (_r2), "r" (_r3), "r" (_r7)
		: "lr", "memory", "cc");
	return ret;
}

static int
lfs_read(const char *path, void *buf, unsigned max)
{
	return svc3(96u, (unsigned)path, (unsigned)buf, max);
}

static int
lfs_write(const char *path, const void *buf, unsigned len)
{
	return svc3(97u, (unsigned)path, (unsigned)buf, len);
}

static int
contains(const unsigned char *buf, int n, const char *needle)
{
	int i, j, m = 0;

	while (needle[m])
		m++;
	for (i = 0; i + m <= n; i++) {
		for (j = 0; j < m; j++)
			if (buf[i + j] != (unsigned char)needle[j])
				break;
		if (j == m)
			return 1;
	}
	return 0;
}

static int
streqn(const unsigned char *a, const char *b, int n)
{
	int i;
	for (i = 0; i < n; i++)
		if (a[i] != (unsigned char)b[i])
			return 0;
	return 1;
}

static int fails;

static void
check(const char *what, int ok)
{
	if (!ok) {
		fails++;
		puts("UCREDSTORE: FAIL ");
		puts(what);
		puts("\n");
	}
}

__attribute__((section(".text.start")))
void
_start(void)
{
	static unsigned char raw[640], buf[640], snap[640];
	static const char secret[] = "s3cr3t-value-0123456789";
	static const char legacy[] = "legacy-key-123";
	int n, snapn, mask;

	puts("ucred: B5.5 credential store test\n");
	(void)svc3(94u, 0u, 0u, 0u);	/* lfs mount */

	/* 0: kernel self-test */
	mask = cred4(9u, "", buf, 0u);
	check("selftest bitmask", mask == 0xF);

	/* snapshot cfg-apikey */
	snapn = lfs_read("cfg-apikey", snap, sizeof(snap));

	/* 1: roundtrip */
	check("set", cred4(0u, "testcred", (void *)secret,
	    sizeof(secret) - 1u) == 0);
	n = cred4(1u, "testcred", buf, sizeof(buf));
	check("get roundtrip", n == (int)(sizeof(secret) - 1u) &&
	    streqn(buf, secret, n));

	/* 2: raw file is a QSEC blob without plaintext */
	n = lfs_read("cfg-testcred", raw, sizeof(raw));
	check("raw is QSEC", n > 33 && raw[0] == 'Q' && raw[1] == 'S' &&
	    raw[2] == 'E' && raw[3] == 'C');
	check("raw has no plaintext", n > 0 &&
	    !contains(raw, n, "s3cr3t-value"));

	/* 3: tamper -> get fails */
	if (n > 40) {
		raw[20] ^= 0x40u;
		(void)lfs_write("cfg-testcred", raw, (unsigned)n);
		check("tamper rejected",
		    cred4(1u, "testcred", buf, sizeof(buf)) == -1);
	}

	/* 5: clear */
	check("clear", cred4(3u, "testcred", buf, 0u) == 0);
	check("present after clear",
	    cred4(2u, "testcred", buf, 0u) == 0);

	/* 4: legacy migration on cfg-apikey */
	(void)lfs_write("cfg-apikey", legacy, sizeof(legacy) - 1u);
	n = cred4(1u, "apikey", buf, sizeof(buf));
	check("legacy get", n == (int)(sizeof(legacy) - 1u) &&
	    streqn(buf, legacy, n));
	n = lfs_read("cfg-apikey", raw, sizeof(raw));
	check("legacy migrated to QSEC", n > 33 && raw[0] == 'Q' &&
	    raw[1] == 'S' && raw[2] == 'E' && raw[3] == 'C' &&
	    !contains(raw, n, "legacy-key"));

	/* restore cfg-apikey */
	if (snapn >= 0)
		(void)lfs_write("cfg-apikey", snap, (unsigned)snapn);
	else
		(void)lfs_write("cfg-apikey", "", 0u);

	if (fails == 0)
		puts("UCREDSTORE: PASS\n");
	sys_exit(0);
}
