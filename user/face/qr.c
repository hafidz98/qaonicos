/*
 * qr.c -- QR encoder (ISO 18004 subset): byte mode, ECC level M,
 * versions 1..6. Written for the QaonicOS face setup screen
 * (ONB-2). Tables are the standard's per-version parameters for
 * level M; GF(256) uses the primitive polynomial 0x11D.
 */

#include "qr.h"

/* --- per-version parameters, ECC M (index = version) --- */
/* total codewords / ec per block / block count / data per block */
static const unsigned char qr_total[7]  = {0, 26, 44, 70, 100, 134, 172};
static const unsigned char qr_ecper[7]  = {0, 10, 16, 26, 18, 24, 16};
static const unsigned char qr_nblk[7]   = {0, 1, 1, 1, 2, 2, 4};
static const unsigned char qr_align[7]  = {0, 0, 18, 22, 26, 30, 34};
static const unsigned char qr_rem[7]    = {0, 0, 7, 7, 7, 7, 7};

#define QR_MAXN	41	/* version 6 */
static unsigned char qr_m[QR_MAXN][QR_MAXN];	/* 1 = dark */
static unsigned char qr_fn[QR_MAXN][QR_MAXN];	/* function modules */
static int qr_n;

int
qr_module(int x, int y)
{
	if (x < 0 || y < 0 || x >= qr_n || y >= qr_n)
		return 0;
	return qr_m[y][x];
}

/* --- GF(256) --- */
static unsigned char gf_exp[512], gf_log[256];
static int gf_ready;

static void
gf_init(void)
{
	int i;
	unsigned int x = 1;
	if (gf_ready)
		return;
	for (i = 0; i < 255; i++) {
		gf_exp[i] = (unsigned char)x;
		gf_log[x] = (unsigned char)i;
		x <<= 1;
		if (x & 0x100u)
			x ^= 0x11Du;
	}
	for (i = 255; i < 512; i++)
		gf_exp[i] = gf_exp[i - 255];
	gf_ready = 1;
}

static unsigned char
gf_mul(unsigned char a, unsigned char b)
{
	if (!a || !b)
		return 0;
	return gf_exp[gf_log[a] + gf_log[b]];
}

/* Reed-Solomon ECC: out[] gets ec parity bytes for data[0..len). */
static void
rs_ecc(const unsigned char *data, int len, int ec, unsigned char *out)
{
	/* generator polynomial coefficients, highest degree first */
	unsigned char gen[32];
	int i, j;

	for (i = 0; i <= ec; i++)
		gen[i] = 0;
	gen[0] = 1;
	for (i = 0; i < ec; i++) {
		/* multiply by (x - alpha^i) == (x + alpha^i) */
		for (j = i; j >= 0; j--)
			gen[j + 1] ^= gf_mul(gen[j], gf_exp[i]);
	}
	for (i = 0; i < ec; i++)
		out[i] = 0;
	for (i = 0; i < len; i++) {
		unsigned char f = data[i] ^ out[0];
		for (j = 0; j + 1 < ec; j++)
			out[j] = out[j + 1];
		out[ec - 1] = 0;
		if (f)
			for (j = 0; j < ec; j++)
				out[j] ^= gf_mul(gen[j + 1], f);
	}
}

/* --- bitstream --- */
static unsigned char bs_buf[400];
static int bs_len;	/* bits */

static void
bs_put(unsigned int val, int bits)
{
	int i;
	for (i = bits - 1; i >= 0; i--) {
		int bit = (int)((val >> i) & 1u);
		if (bit)
			bs_buf[bs_len >> 3] |=
			    (unsigned char)(0x80u >> (bs_len & 7));
		bs_len++;
	}
}

/* --- function patterns --- */
static void
set_fn(int x, int y, int dark)
{
	qr_m[y][x] = dark ? 1 : 0;
	qr_fn[y][x] = 1;
}

static void
draw_finder(int ox, int oy)
{
	int x, y;
	for (y = -1; y < 8; y++)
		for (x = -1; x < 8; x++) {
			int px = ox + x, py = oy + y;
			int dark;
			if (px < 0 || py < 0 || px >= qr_n || py >= qr_n)
				continue;
			if (x == -1 || x == 7 || y == -1 || y == 7)
				dark = 0;	/* separator */
			else if (x == 0 || x == 6 || y == 0 || y == 6 ||
			    (x >= 2 && x <= 4 && y >= 2 && y <= 4))
				dark = 1;
			else
				dark = 0;
			set_fn(px, py, dark);
		}
}

static void
draw_functions(int ver)
{
	int i, a = qr_align[ver];

	for (i = 0; i < qr_n; i++) {
		qr_fn[6][i] = 1;
		qr_fn[i][6] = 1;
	}
	for (i = 8; i < qr_n - 8; i++) {
		int d = (i % 2 == 0);
		qr_m[6][i] = d;
		qr_m[i][6] = d;
	}
	draw_finder(0, 0);
	draw_finder(qr_n - 7, 0);
	draw_finder(0, qr_n - 7);
	if (a) {
		int x, y;
		for (y = a - 2; y <= a + 2; y++)
			for (x = a - 2; x <= a + 2; x++) {
				int dx = x - a, dy = y - a;
				int adx = dx < 0 ? -dx : dx;
				int ady = dy < 0 ? -dy : dy;
				int m = adx > ady ? adx : ady;
				set_fn(x, y, m != 1);
			}
	}
	/* format info placeholders + dark module */
	for (i = 0; i < 9; i++) {
		qr_fn[8][i] = 1;
		qr_fn[i][8] = 1;
	}
	for (i = 0; i < 8; i++) {
		qr_fn[8][qr_n - 1 - i] = 1;
		qr_fn[qr_n - 1 - i][8] = 1;
	}
	qr_m[qr_n - 8][8] = 1;	/* dark module */
	qr_fn[qr_n - 8][8] = 1;
}

/* Place the final codeword stream (with remainder bits). */
static void
place_data(const unsigned char *stream, int nbits)
{
	int x = qr_n - 1, y = qr_n - 1, dir = -1, i = 0;

	while (x > 0) {
		if (x == 6)
			x--;
		for (;;) {
			int dx;
			for (dx = 0; dx < 2; dx++) {
				int px = x - dx;
				if (!qr_fn[y][px]) {
					int bit = 0;
					if (i < nbits)
						bit = (stream[i >> 3] >>
						    (7 - (i & 7))) & 1;
					qr_m[y][px] = (unsigned char)bit;
					i++;
				}
			}
			y += dir;
			if (y < 0 || y >= qr_n) {
				y -= dir;
				dir = -dir;
				x -= 2;
				break;
			}
		}
	}
}

static int
mask_bit(int mask, int x, int y)
{
	switch (mask) {
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (x / 3 + y / 2) % 2 == 0;
	case 5: return (x * y) % 2 + (x * y) % 3 == 0;
	case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
	default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
	}
}

static void
apply_mask(int mask)
{
	int x, y;
	for (y = 0; y < qr_n; y++)
		for (x = 0; x < qr_n; x++)
			if (!qr_fn[y][x] && mask_bit(mask, x, y))
				qr_m[y][x] ^= 1;
}

/* Format info for ECC M (indicator 00): computed with the
 * standard (15,5) BCH code + 0x5412 mask, not tabled. */
static unsigned int
format_bits(int mask)
{
	int data = mask, rem = data, i;
	for (i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	return (unsigned int)((data << 10) | rem) ^ 0x5412u;
}

static void
draw_format(int mask)
{
	unsigned int f = format_bits(mask);
	int i;
	/* Placement per the standard mapping (bit i = (f >> i) & 1). */
	for (i = 0; i <= 5; i++)
		set_fn(8, i, (int)((f >> i) & 1u));
	set_fn(8, 7, (int)((f >> 6) & 1u));
	set_fn(8, 8, (int)((f >> 7) & 1u));
	set_fn(7, 8, (int)((f >> 8) & 1u));
	for (i = 9; i < 15; i++)
		set_fn(14 - i, 8, (int)((f >> i) & 1u));
	for (i = 0; i < 8; i++)
		set_fn(qr_n - 1 - i, 8, (int)((f >> i) & 1u));
	for (i = 8; i < 15; i++)
		set_fn(8, qr_n - 15 + i, (int)((f >> i) & 1u));
	set_fn(8, qr_n - 8, 1);	/* dark module */
}

static int
penalty(void)
{
	int score = 0, x, y, i, dark = 0;

	/* rule 1: runs of >= 5 in rows and columns */
	for (y = 0; y < qr_n; y++) {
		int run = 1;
		for (x = 1; x <= qr_n; x++) {
			if (x < qr_n && qr_m[y][x] == qr_m[y][x - 1])
				run++;
			else {
				if (run >= 5)
					score += run - 2;
				run = 1;
			}
		}
	}
	for (x = 0; x < qr_n; x++) {
		int run = 1;
		for (y = 1; y <= qr_n; y++) {
			if (y < qr_n && qr_m[y][x] == qr_m[y - 1][x])
				run++;
			else {
				if (run >= 5)
					score += run - 2;
				run = 1;
			}
		}
	}
	/* rule 2: 2x2 blocks */
	for (y = 0; y + 1 < qr_n; y++)
		for (x = 0; x + 1 < qr_n; x++)
			if (qr_m[y][x] == qr_m[y][x + 1] &&
			    qr_m[y][x] == qr_m[y + 1][x] &&
			    qr_m[y][x] == qr_m[y + 1][x + 1])
				score += 3;
	/* rule 3: finder-like 1:1:3:1:1 with 4-module light guard */
	{
		static const unsigned char pat[11] =
		    {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
		static const unsigned char pat2[11] =
		    {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
		for (y = 0; y < qr_n; y++)
			for (x = 0; x + 11 <= qr_n; x++) {
				int m1 = 1, m2 = 1;
				for (i = 0; i < 11; i++) {
					if (qr_m[y][x + i] != pat[i])
						m1 = 0;
					if (qr_m[y][x + i] != pat2[i])
						m2 = 0;
				}
				if (m1 || m2)
					score += 40;
			}
		for (x = 0; x < qr_n; x++)
			for (y = 0; y + 11 <= qr_n; y++) {
				int m1 = 1, m2 = 1;
				for (i = 0; i < 11; i++) {
					if (qr_m[y + i][x] != pat[i])
						m1 = 0;
					if (qr_m[y + i][x] != pat2[i])
						m2 = 0;
				}
				if (m1 || m2)
					score += 40;
			}
	}
	/* rule 4: dark ratio deviation */
	for (y = 0; y < qr_n; y++)
		for (x = 0; x < qr_n; x++)
			dark += qr_m[y][x];
	{
		int total = qr_n * qr_n;
		int pct = dark * 100 / total;
		int lo = (pct / 5) * 5, hi = lo + 5;
		int d1 = lo > 50 ? lo - 50 : 50 - lo;
		int d2 = hi > 50 ? hi - 50 : 50 - hi;
		score += (d1 < d2 ? d1 : d2) / 5 * 10;
	}
	return score;
}

int
qr_encode(const char *text)
{
	int len = 0, ver, i, j;
	unsigned char data[200];

	gf_init();
	while (text[len])
		len++;
	for (ver = 1; ver <= 6; ver++) {
		int datacw = qr_total[ver] -
		    qr_ecper[ver] * qr_nblk[ver];
		if (len <= datacw - 2)
			break;
	}
	if (ver > 6)
		return -1;
	{
		int datacw = qr_total[ver] - qr_ecper[ver] * qr_nblk[ver];
		int per = datacw / qr_nblk[ver];
		unsigned char ecc[108], stream[200];
		int nbits_stream;

		/* build data codewords */
		for (i = 0; i < (int)sizeof(bs_buf); i++)
			bs_buf[i] = 0;
		bs_len = 0;
		bs_put(0x4u, 4);		/* byte mode */
		bs_put((unsigned int)len, 8);	/* count (v <= 9) */
		for (i = 0; i < len; i++)
			bs_put((unsigned char)text[i], 8);
		{
			int capbits = datacw * 8;
			int term = capbits - bs_len;
			if (term > 4)
				term = 4;
			bs_put(0, term);
			while (bs_len % 8)
				bs_put(0, 1);
			for (i = 0; bs_len < capbits; i++)
				bs_put((i % 2 == 0) ? 0xECu : 0x11u, 8);
		}
		for (i = 0; i < datacw; i++)
			data[i] = bs_buf[i];
		/* ecc per block */
		for (i = 0; i < qr_nblk[ver]; i++)
			rs_ecc(data + i * per, per, qr_ecper[ver],
			    ecc + i * qr_ecper[ver]);
		/* interleave data then ecc */
		{
			int pos = 0;
			for (j = 0; j < per; j++)
				for (i = 0; i < qr_nblk[ver]; i++)
					stream[pos++] = data[i * per + j];
			for (j = 0; j < qr_ecper[ver]; j++)
				for (i = 0; i < qr_nblk[ver]; i++)
					stream[pos++] =
					    ecc[i * qr_ecper[ver] + j];
			nbits_stream = pos * 8 + qr_rem[ver];
			/* tail bits of stream buffer for remainder */
			if (pos < (int)sizeof(stream))
				stream[pos] = 0;
			qr_n = 17 + 4 * ver;
			for (i = 0; i < qr_n; i++)
				for (j = 0; j < qr_n; j++)
					qr_m[i][j] = qr_fn[i][j] = 0;
			draw_functions(ver);
			place_data(stream, nbits_stream);
		}
		/* choose best mask */
		{
			unsigned char base[QR_MAXN][QR_MAXN];
			unsigned char bestm[QR_MAXN][QR_MAXN];
			int best = -1, bestscore = 0, mask;
			for (i = 0; i < qr_n; i++)
				for (j = 0; j < qr_n; j++)
					base[i][j] = qr_m[i][j];
			for (mask = 0; mask < 8; mask++) {
				int sc;
				for (i = 0; i < qr_n; i++)
					for (j = 0; j < qr_n; j++)
						qr_m[i][j] = base[i][j];
				apply_mask(mask);
				draw_format(mask);
				sc = penalty();
				if (best < 0 || sc < bestscore) {
					best = mask;
					bestscore = sc;
					for (i = 0; i < qr_n; i++)
						for (j = 0; j < qr_n; j++)
							bestm[i][j] = qr_m[i][j];
				}
			}
			for (i = 0; i < qr_n; i++)
				for (j = 0; j < qr_n; j++)
					qr_m[i][j] = bestm[i][j];
			(void)best;
		}
	}
	return qr_n;
}

