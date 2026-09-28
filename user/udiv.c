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
