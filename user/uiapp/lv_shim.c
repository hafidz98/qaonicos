/*
 * lv_shim.c - freestanding libcalls for the LVGL build (UI-1).
 *
 * Userspace links with -nostdlib and only ulib + udiv, so
 * compiler-generated calls to memset/memmove (and the
 * __aeabi_memclr family clang emits for zeroing) need a
 * home. LVGL itself uses its builtin stdlib (lv_memset
 * etc.), so these exist only for libcalls the compiler
 * emits, not for LVGL source. Kept deliberately small:
 * add a symbol here only when the link demands it.
 */

typedef unsigned long size_t;

void *
memset(void *dst, int c, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	size_t i;
	for (i = 0; i < n; i++)
		d[i] = (unsigned char)c;
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	size_t i;
	if (d <= s) {
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		for (i = n; i > 0; i--)
			d[i - 1] = s[i - 1];
	}
	return dst;
}

void
__aeabi_memclr(void *dst, size_t n)
{
	memset(dst, 0, n);
}

void
__aeabi_memclr4(void *dst, size_t n)
{
	memset(dst, 0, n);
}

void
__aeabi_memclr8(void *dst, size_t n)
{
	memset(dst, 0, n);
}

void *
__aeabi_memcpy4(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	size_t i;
	for (i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

/* __aeabi_idivmod: quotient in r0, remainder in r1
 * (ARM EABI). Computed via the udiv helpers; the
 * remainder is placed in r1 by inline asm just before
 * return (the epilogue does not touch r1). */
int
__aeabi_idivmod(int n, int d)
{
	extern int __aeabi_idiv(int n, int d);
	int q = __aeabi_idiv(n, d);
	int r = n - q * d;
	__asm__ volatile("mov r1, %0" :: "r" (r) : "r1");
	return q;
}

/* 64-bit divmod, returned as a 16-byte struct: q in
 * r0-r1, r in r2-r3, exactly the register convention
 * the compiler's __aeabi_uldivmod / __aeabi_ldivmod
 * call sites expect. Shift-subtract long division;
 * divide-by-zero yields 0 (callers never divide by
 * zero; this only avoids a hang). */
typedef struct {
	unsigned long long q, r;
} ulldiv_t;

ulldiv_t
__aeabi_uldivmod(unsigned long long n, unsigned long long d)
{
	ulldiv_t out;
	unsigned long long q = 0, r = 0;
	int i;
	if (d == 0) {
		out.q = 0;
		out.r = 0;
		return out;
	}
	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1u);
		if (r >= d) {
			r -= d;
			q |= (1ull << i);
		}
	}
	out.q = q;
	out.r = r;
	return out;
}

typedef struct {
	long long q, r;
} lldiv_t;

lldiv_t
__aeabi_ldivmod(long long n, long long d)
{
	lldiv_t out;
	ulldiv_t u;
	unsigned long long un, ud;
	int negq, negr;
	long long q, r;
	if (d == 0) {
		out.q = 0;
		out.r = 0;
		return out;
	}
	negq = (n < 0) ^ (d < 0);
	negr = (n < 0);
	un = n < 0 ? (unsigned long long)(-(n + 1)) + 1u :
	    (unsigned long long)n;
	ud = d < 0 ? (unsigned long long)(-(d + 1)) + 1u :
	    (unsigned long long)d;
	u = __aeabi_uldivmod(un, ud);
	q = (long long)u.q;
	if (negq)
		q = -q;
	r = (long long)u.r;
	if (negr)
		r = -r;
	out.q = q;
	out.r = r;
	return out;
}
