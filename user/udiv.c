/*
 * udiv.c - Implementasi __aeabi_uidiv/uidivmod untuk program userspace
 * (Fase C).  Dibutuhkan karena umon.c memakai pembagian unsigned
 * (persentase mem/disk); program user di-link tanpa libgcc.
 *
 * Algoritma: binary long division (disalin dari kernel aeabi.c).
 */
unsigned int
__aeabi_uidiv(unsigned int n, unsigned int d)
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

/*
 * App A1: face.c memakai pembagian/modulo SIGNED (interpolasi easing,
 * gaze acak).  Implementasi via versi unsigned di atas.
 */
int
__aeabi_idiv(int n, int d)
{
	int neg = (n < 0) ^ (d < 0);
	unsigned int un = (n < 0) ? (unsigned int)(-(n + 1)) + 1u
				  : (unsigned int)n;
	unsigned int ud = (d < 0) ? (unsigned int)(-(d + 1)) + 1u
				  : (unsigned int)d;
	unsigned int q = __aeabi_uidiv(un, ud);

	if (neg)
		return -(int)q;
	return (int)q;
}

int
__aeabi_imod(int n, int d)
{
	return n - __aeabi_idiv(n, d) * d;
}

/*
 * App A1: face.c menyalin struct (face_state_t) -> compiler emit
 * __aeabi_memcpy8.  Implementasi byte-accurate.
 */
void
__aeabi_memcpy8(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	while (n-- > 0)
		*d++ = *s++;
}

void
__aeabi_memcpy(void *dst, const void *src, unsigned int n)
{
	__aeabi_memcpy8(dst, src, n);
}
