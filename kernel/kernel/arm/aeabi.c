/*
 * mach3/kernel/arm/aeabi.c -- ARM EABI helper routines.
 * Minimal implementations for freestanding kernel (no libgcc).
 */

unsigned int
__aeabi_uidiv(unsigned int n, unsigned int d)
{
	unsigned int q = 0;
	unsigned int r = 0;
	int i;

	if (d == 0)
		return 0;	/* avoid fault; kernel should not divide by 0 */

	for (i = 31; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1u);
		if (r >= d) {
			r -= d;
			q |= (1u << i);
		}
	}
	return q;
}

unsigned long long
__aeabi_uidivmod(unsigned int n, unsigned int d)
{
	unsigned int q = 0;
	unsigned int r = 0;
	int i;

	if (d == 0)
		return 0;

	for (i = 31; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1u);
		if (r >= d) {
			r -= d;
			q |= (1u << i);
		}
	}
	return ((unsigned long long)r << 32) | q;
}

void
__aeabi_memcpy4(void *dst, const void *src, unsigned int n)
{
	unsigned int *d = (unsigned int *)dst;
	const unsigned int *s = (const unsigned int *)src;
	while (n >= 4) {
		*d++ = *s++;
		n -= 4;
	}
}

unsigned long long
__aeabi_uldivmod(unsigned long long n, unsigned long long d)
{
	unsigned long long q = 0;
	unsigned long long r = 0;
	int i;

	if (d == 0)
		return 0;

	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1ull);
		if (r >= d) {
			r -= d;
			q |= (1ull << i);
		}
	}
	return (r << 32) | (q & 0xffffffffull); /* simplified */
}

int
__aeabi_idiv(int n, int d)
{
	int neg = 0;
	unsigned int un, ud, q;

	if (d == 0)
		return 0;
	if (n < 0) { neg = !neg; un = (unsigned int)(-n); }
	else un = (unsigned int)n;
	if (d < 0) { neg = !neg; ud = (unsigned int)(-d); }
	else ud = (unsigned int)d;

	q = __aeabi_uidiv(un, ud);
	return neg ? -(int)q : (int)q;
}

long long
__aeabi_idivmod(int n, int d)
{
	/* returns {rem, quot} packed; simplified */
	int q = __aeabi_idiv(n, d);
	int r = n - q * d;
	return ((long long)r << 32) | (unsigned int)q;
}

void
__aeabi_memset4(void *dst, unsigned int n, int c)
{
	unsigned int *d = (unsigned int *)dst;
	unsigned int v = (unsigned char)c;
	v |= v << 8; v |= v << 16;
	while (n >= 4) {
		*d++ = v;
		n -= 4;
	}
}
