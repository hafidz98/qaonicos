/*
 * user/tls/tls_port.c -- Port mbedTLS ke bare-metal QaonicOS (Q2b).
 *
 * Menyediakan: mini-libc (memcpy/memmove/memset/memcmp/strlen/
 * snprintf/printf), bump allocator untuk MBEDTLS_PLATFORM_MEMORY,
 * mbedtls_time() (via sys_time_get), dan mbedtls_hardware_poll().
 *
 * CATATAN JUJUR: mbedtls_hardware_poll() di bawah memakai timer +
 * xorshift — LEMAH, hanya untuk uji Q2 di QEMU. Di hardware nyata
 * butuh sumber entropi yang layak (TRNG RV1103 — riset terpisah).
 */

#include "ulib/ulib.h"
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Mini-libc.                                                          */
/* ------------------------------------------------------------------ */

void *
memcpy(void *dst, const void *src, unsigned n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	unsigned i;

	for (i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

void *
memmove(void *dst, const void *src, unsigned n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	unsigned i;

	if (d < s) {
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else if (d > s) {
		for (i = n; i > 0; i--)
			d[i - 1] = s[i - 1];
	}
	return dst;
}

void *
memset(void *dst, int c, unsigned n)
{
	unsigned char *d = (unsigned char *)dst;
	unsigned i;

	for (i = 0; i < n; i++)
		d[i] = (unsigned char)c;
	return dst;
}

int
memcmp(const void *a, const void *b, unsigned n)
{
	const unsigned char *p = (const unsigned char *)a;
	const unsigned char *q = (const unsigned char *)b;
	unsigned i;

	for (i = 0; i < n; i++) {
		if (p[i] != q[i])
			return (int)p[i] - (int)q[i];
	}
	return 0;
}

unsigned
strlen(const char *s)
{
	unsigned n = 0;

	while (s[n] != '\0')
		n++;
	return n;
}

/* vsnprintf minimal: %s %d %i %u %x %X %c %% %p + width (mis. %04x)
 * + modifier l/z. Cukup untuk kebutuhan mbedTLS. */
int
vsnprintf(char *out, unsigned outlen, const char *fmt, va_list ap)
{
	char *o = out;
	char *end = out + (outlen > 0 ? outlen - 1 : 0);
	unsigned long uv;
	long sv;
	unsigned neg, base, width, i, ndig;
	char dig[32];
	const char *s;
	char pad;

#define EMIT(c) do { if (o < end) *o++ = (char)(c); } while (0)

	while (*fmt != '\0') {
		if (*fmt != '%') {
			EMIT(*fmt++);
			continue;
		}
		fmt++;
		/* modifier l/z (abaikan, arg dibaca 32-bit) */
		while (*fmt == 'l' || *fmt == 'z')
			fmt++;
		/* width (angka saja; presisi diabaikan) */
		width = 0;
		pad = ' ';
		if (*fmt == '0') {
			pad = '0';
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10u + (unsigned)(*fmt - '0');
			fmt++;
		}
		while (*fmt == '.' || (*fmt >= '0' && *fmt <= '9'))
			fmt++;		/* presisi: abaikan */
		neg = 0;
		s = 0;
		switch (*fmt) {
		case 's':
			s = __builtin_va_arg(ap, const char *);
			if (s == 0)
				s = "(null)";
			while (*s != '\0')
				EMIT(*s++);
			break;
		case 'd':
		case 'i':
			sv = (long)__builtin_va_arg(ap, int);
			base = 10;
			if (sv < 0) {
				neg = 1;
				uv = (unsigned long)(-sv);
			} else {
				uv = (unsigned long)sv;
			}
			goto num;
		case 'u':
			uv = (unsigned long)__builtin_va_arg(ap, unsigned);
			base = 10;
			goto num;
		case 'x':
		case 'X':
		case 'p':
			if (*fmt == 'p')
				uv = (unsigned long)__builtin_va_arg(ap, void *);
			else
				uv = (unsigned long)__builtin_va_arg(ap, unsigned);
			base = 16;
			goto num;
		case 'c':
			EMIT(__builtin_va_arg(ap, int));
			break;
		case '%':
			EMIT('%');
			break;
		default:
			EMIT('%');
			EMIT(*fmt);
			break;
		}
		fmt++;
		continue;
	num:
		ndig = 0;
		if (uv == 0) {
			dig[ndig++] = '0';
		} else {
			while (uv > 0 && ndig < sizeof(dig)) {
				unsigned d = (unsigned)(uv % base);
				dig[ndig++] = (char)(d < 10 ? '0' + d :
					(*fmt == 'X' ? 'A' : 'a') + d - 10);
				uv /= base;
			}
		}
		if (neg)
			EMIT('-');
		while (ndig + (neg ? 1 : 0) < width) {
			EMIT(pad);
			width--;
		}
		while (ndig > 0)
			EMIT(dig[--ndig]);
		fmt++;
	}
	if (outlen > 0)
		*o = '\0';
	return (int)(o - out);
#undef EMIT
}

int
snprintf(char *out, unsigned outlen, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(out, outlen, fmt, ap);
	va_end(ap);
	return r;
}

int
printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	puts(buf);
	return r;
}

/* ------------------------------------------------------------------ */
/* Bump allocator untuk MBEDTLS_PLATFORM_MEMORY.                       */
/*                                                                     */
/* Disiplin: tls_pool_reset() dipanggil di awal tiap transaksi HTTPS   */
/* setelah konteks mbedTLS di-free. Karena free() = no-op, semua alokasi */
/* satu transaksi direklamasi sekaligus saat reset.                    */
/* ------------------------------------------------------------------ */

#define TLS_POOL_SIZE	(512u * 1024u)	/* Q2b: free-list allocator, 512KB cukup */

/* Free-list allocator sederhana untuk mbedTLS. */
struct tls_block {
	unsigned size;		/* ukuran payload (tidak termasuk header) */
	struct tls_block *next;
};

static unsigned char tls_pool[TLS_POOL_SIZE];
static struct tls_block *tls_free_list;
static unsigned tls_pool_peak;
static unsigned tls_pool_used_now;

static void
tls_pool_init(void)
{
	struct tls_block *b = (struct tls_block *)tls_pool;
	b->size = TLS_POOL_SIZE - sizeof(*b);
	b->next = 0;
	tls_free_list = b;
	tls_pool_used_now = 0;
	tls_pool_peak = 0;
}

void *
tls_calloc(unsigned n, unsigned size)
{
	unsigned total = n * size;
	struct tls_block **pp, *b, *nb;
	void *p;

	total = (total + 7u) & ~7u;	/* align 8 */
	if (tls_free_list == 0)
		tls_pool_init();

	/* Cari block yang cukup (first-fit). */
	for (pp = &tls_free_list; *pp != 0; pp = &(*pp)->next) {
		b = *pp;
		if (b->size >= total) {
			/* Split jika sisa cukup untuk header + 8 byte. */
			if (b->size >= total + sizeof(*b) + 8u) {
				nb = (struct tls_block *)((unsigned char *)(b + 1) + total);
				nb->size = b->size - total - sizeof(*b);
				nb->next = b->next;
				b->size = total;
				b->next = nb;
			}
			*pp = b->next;	/* keluarkan dari free list */
			p = (void *)(b + 1);
			memset(p, 0, total);
			tls_pool_used_now += total + sizeof(*b);
			if (tls_pool_used_now > tls_pool_peak)
				tls_pool_peak = tls_pool_used_now;
			return p;
		}
	}
	return 0;	/* out of memory */
}

void
tls_free(void *p)
{
	struct tls_block *b, **pp;

	if (p == 0)
		return;
	b = ((struct tls_block *)p) - 1;
	tls_pool_used_now -= b->size + sizeof(*b);

	/* Masukkan kembali ke free list (sorted by address untuk coalesce). */
	for (pp = &tls_free_list; *pp != 0 && *pp < b; pp = &(*pp)->next)
		;
	b->next = *pp;
	*pp = b;

	/* Coalesce dengan next jika bersebelahan. */
	if (b->next != 0 &&
	    (unsigned char *)(b + 1) + b->size == (unsigned char *)b->next) {
		b->size += sizeof(*b) + b->next->size;
		b->next = b->next->next;
	}
}

void
tls_pool_reset(void)
{
	tls_pool_init();
}

unsigned
tls_pool_max(void)
{
	return tls_pool_peak;
}

/* ------------------------------------------------------------------ */
/* Waktu & entropi.                                                    */
/* ------------------------------------------------------------------ */

/* LEMAH (lihat catatan di atas): timer + xorshift. Hanya untuk uji. */
int
mbedtls_hardware_poll(void *data, unsigned char *output, unsigned len,
		       unsigned *olen)
{
	static unsigned state = 0x12345678u;
	unsigned i;

	(void)data;
	state ^= sys_uptime() + 0x9e3779b9u;
	for (i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		state += sys_uptime() * 2654435761u + i * 97u;
		output[i] = (unsigned char)(state >> 24);
	}
	*olen = len;
	return 0;
}

/* Tambahan string.h/stdlib.h untuk mbedTLS. */

int
strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *
strstr(const char *h, const char *n)
{
	unsigned nl = strlen(n);

	if (nl == 0)
		return (char *)h;
	while (*h != '\0') {
		unsigned i;
		for (i = 0; i < nl; i++) {
			if (h[i] != n[i])
				break;
		}
		if (i == nl)
			return (char *)h;
		h++;
	}
	return 0;
}

char *
strchr(const char *s, int c)
{
	while (*s != '\0') {
		if (*s == (char)c)
			return (char *)s;
		s++;
	}
	return 0;
}

/* rand() lemah (xorshift); hanya dipakai jalur non-kritis. */
static unsigned tls_rand_state = 0x9e3779b9u;

int
rand(void)
{
	unsigned x = tls_rand_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	tls_rand_state = x;
	return (int)(x & 0x7fffffffu);
}

void
srand(unsigned seed)
{
	tls_rand_state = seed ? seed : 0x9e3779b9u;
}

void
tls_assert_fail(const char *expr, const char *file, int line)
{
	char tmp[32];

	puts("TLS ASSERT: ");
	puts(expr);
	puts(" @ ");
	puts(file);
	puts(":");
	snprintf(tmp, sizeof(tmp), "%d\n", line);
	puts(tmp);
	sys_exit(1);
	for (;;)
		sys_yield();
}

/* Waktu untuk mbedTLS (lihat MBEDTLS_PLATFORM_STD_TIME di config). */
long
tls_time_get(long *t)
{
	/* Q2b SEMENTARA: bypass sys_time_get (syscall TIME_SET bermasalah);
	 * pakai waktu fixed dalam masa berlaku cert (2026-09-29 s/d 2036). */
	long now = 1790683200L;	/* 2026-09-29 12:00 UTC */

	if (t != 0)
		*t = now;
	return now;
}

/* mbedtls_ms_time() (MBEDTLS_PLATFORM_MS_TIME_ALT). */
long long
mbedtls_ms_time(void)
{
	return (long long)sys_uptime() * 1000LL;
}

/* Helper EABI untuk struct copy yang di-emit clang (sha256_clone). */
void
__aeabi_memcpy4(void *dst, const void *src, unsigned n)
{
	memcpy(dst, src, n);
}

/* Helper EABI tambahan yang di-emit clang. */
void
__aeabi_memclr8(void *dst, unsigned n)
{
	memset(dst, 0, n);
}

/* __aeabi_uldivmod: divisi 64-bit. AAPCS: n di r0/r1, d di r2/r3;
 * kembali q di r0/r1, sisa di r2/r3. Dipakai bignum.c. */
__asm__(
".global __aeabi_uldivmod\n"
".type __aeabi_uldivmod, %function\n"
"__aeabi_uldivmod:\n"
"	push	{r4-r7, lr}\n"
"	mov	r4, #0			@ q_lo\n"
"	mov	r5, #0			@ q_hi\n"
"	mov	r6, #0			@ r_lo\n"
"	mov	r7, #0			@ r_hi\n"
"	mov	ip, #64\n"
"1:\n"
"	lsls	r0, r0, #1		@ n <<= 1 (pair)\n"
"	adcs	r1, r1, r1		@ C = bit MSB n\n"
"	adcs	r6, r6, r6		@ r = (r << 1) | bit\n"
"	adc	r7, r7, r7\n"
"	lsls	r4, r4, #1		@ q <<= 1 (pair)\n"
"	adc	r5, r5, r5\n"
"	cmp	r7, r3			@ r >= d ?\n"
"	bhi	2f\n"
"	blo	3f\n"
"	cmp	r6, r2\n"
"	blo	3f\n"
"2:\n"
"	subs	r6, r6, r2		@ r -= d\n"
"	sbc	r7, r7, r3\n"
"	orr	r4, r4, #1		@ q |= 1\n"
"3:\n"
"	subs	ip, ip, #1\n"
"	bne	1b\n"
"	mov	r0, r4			@ q -> r0/r1\n"
"	mov	r1, r5\n"
"	mov	r2, r6			@ sisa -> r2/r3\n"
"	mov	r3, r7\n"
"	pop	{r4-r7, pc}\n"
);
