/*
 * aesgcm.c -- AES-256-GCM, self-contained (B5.5).
 *
 * AES: FIPS-197, encryption direction only (GCM is CTR-based).
 * State is column-major per the standard (s[r][c] = in[r + 4c]).
 * GCM: SP 800-38D with 96-bit nonces (J0 = nonce || 1).
 * GHASH multiplication follows the standard's bit ordering
 * (the "weird" reflected convention) -- implemented as the
 * classic shift-and-reduce over the 128-bit block.
 */

#include "aesgcm.h"

static const unsigned char sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/* --- AES-256 block encryption --- */

static unsigned char
xtime(unsigned char x)
{
	return (unsigned char)((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0u));
}

struct aes256 {
	unsigned char rk[240];	/* 15 round keys */
};

static void
aes256_init(struct aes256 *a, const unsigned char key[32])
{
	static const unsigned char rcon[7] =
	    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};
	int i;

	for (i = 0; i < 32; i++)
		a->rk[i] = key[i];
	for (i = 8; i < 60; i++) {
		unsigned char t[4];
		int w = (i - 1) * 4;
		t[0] = a->rk[w];
		t[1] = a->rk[w + 1];
		t[2] = a->rk[w + 2];
		t[3] = a->rk[w + 3];
		if (i % 8 == 0) {
			unsigned char x = t[0];
			t[0] = (unsigned char)(sbox[t[1]] ^ rcon[i / 8 - 1]);
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[x];
		} else if (i % 8 == 4) {
			t[0] = sbox[t[0]];
			t[1] = sbox[t[1]];
			t[2] = sbox[t[2]];
			t[3] = sbox[t[3]];
		}
		w = i * 4;
		a->rk[w]     = (unsigned char)(a->rk[w - 32] ^ t[0]);
		a->rk[w + 1] = (unsigned char)(a->rk[w - 31] ^ t[1]);
		a->rk[w + 2] = (unsigned char)(a->rk[w - 30] ^ t[2]);
		a->rk[w + 3] = (unsigned char)(a->rk[w - 29] ^ t[3]);
	}
}

static void
aes256_encrypt_block(const struct aes256 *a,
    const unsigned char in[16], unsigned char out[16])
{
	unsigned char s[16];
	int i, round;

	for (i = 0; i < 16; i++)
		s[i] = (unsigned char)(in[i] ^ a->rk[i]);
	for (round = 1; round <= 14; round++) {
		unsigned char t[16];
		/* SubBytes + ShiftRows: out[r][c] = sbox(s[r][(c+r)%4]) */
		for (i = 0; i < 16; i++) {
			int r = i % 4, c = i / 4;
			t[i] = sbox[s[r + 4 * ((c + r) % 4)]];
		}
		/* MixColumns (skipped in the last round) */
		if (round < 14) {
			for (i = 0; i < 4; i++) {
				unsigned char *col = &t[4 * i];
				unsigned char a0 = col[0], a1 = col[1],
				    a2 = col[2], a3 = col[3];
				col[0] = (unsigned char)(xtime(a0) ^
				    (xtime(a1) ^ a1) ^ a2 ^ a3);
				col[1] = (unsigned char)(a0 ^ xtime(a1) ^
				    (xtime(a2) ^ a2) ^ a3);
				col[2] = (unsigned char)(a0 ^ a1 ^
				    xtime(a2) ^ (xtime(a3) ^ a3));
				col[3] = (unsigned char)((xtime(a0) ^ a0) ^
				    a1 ^ a2 ^ xtime(a3));
			}
		}
		for (i = 0; i < 16; i++)
			s[i] = (unsigned char)(t[i] ^ a->rk[round * 16 + i]);
	}
	for (i = 0; i < 16; i++)
		out[i] = s[i];
}

/* --- GHASH --- */

static void
ghash_mul(unsigned char y[16], const unsigned char h[16])
{
	unsigned char z[16], v[16];
	int i, j;

	for (i = 0; i < 16; i++) {
		z[i] = 0;
		v[i] = h[i];	/* V starts at H; bits are taken from y */
	}
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 8; j++) {
			int bit = (y[i] >> (7 - j)) & 1;
			int k, lsb;
			if (bit)
				for (k = 0; k < 16; k++)
					z[k] ^= v[k];
			lsb = v[15] & 1;
			for (k = 15; k > 0; k--)
				v[k] = (unsigned char)((v[k] >> 1) |
				    ((v[k - 1] & 1) << 7));
			v[0] >>= 1;
			if (lsb)
				v[0] ^= 0xe1u;
		}
	}
	for (i = 0; i < 16; i++)
		y[i] = z[i];
}

static void
ghash_update(unsigned char y[16], const unsigned char h[16],
    const unsigned char *data, unsigned int len)
{
	unsigned int i;
	int k;

	while (len >= 16u) {
		for (k = 0; k < 16; k++)
			y[k] ^= data[k];
		ghash_mul(y, h);
		data += 16;
		len -= 16u;
	}
	if (len) {
		for (i = 0; i < len; i++)
			y[i] ^= data[i];
		ghash_mul(y, h);
	}
}

static void
gctr(const struct aes256 *a, const unsigned char j0[16],
    const unsigned char *in, unsigned int len, unsigned char *out)
{
	unsigned char cb[16], ks[16];
	unsigned int i;
	int k;

	for (k = 0; k < 16; k++)
		cb[k] = j0[k];
	/* first block uses inc32(J0) */
	for (i = 0; i < len; i += 16u) {
		unsigned int carry;
		int n = (int)(len - i < 16u ? len - i : 16u);
		/* inc32 */
		carry = 1;
		for (k = 15; k >= 12; k--) {
			unsigned int v = cb[k] + carry;
			cb[k] = (unsigned char)v;
			carry = v >> 8;
		}
		aes256_encrypt_block(a, cb, ks);
		for (k = 0; k < n; k++)
			out[i + k] = (unsigned char)(in[i + k] ^ ks[k]);
	}
}

static void
gcm_tag(const struct aes256 *a, const unsigned char h[16],
    const unsigned char j0[16], const unsigned char *aad,
    unsigned int aad_len, const unsigned char *ct, unsigned int len,
    unsigned char tag[16])
{
	unsigned char y[16], lenblk[16], ej0[16];
	unsigned int bits;
	int k;

	for (k = 0; k < 16; k++)
		y[k] = 0;
	if (aad_len)
		ghash_update(y, h, aad, aad_len);
	if (len)
		ghash_update(y, h, ct, len);
	for (k = 0; k < 16; k++)
		lenblk[k] = 0;
	bits = aad_len * 8u;
	lenblk[4] = (unsigned char)(bits >> 24);
	lenblk[5] = (unsigned char)(bits >> 16);
	lenblk[6] = (unsigned char)(bits >> 8);
	lenblk[7] = (unsigned char)bits;
	bits = len * 8u;
	lenblk[12] = (unsigned char)(bits >> 24);
	lenblk[13] = (unsigned char)(bits >> 16);
	lenblk[14] = (unsigned char)(bits >> 8);
	lenblk[15] = (unsigned char)bits;
	for (k = 0; k < 16; k++)
		y[k] ^= lenblk[k];
	ghash_mul(y, h);
	aes256_encrypt_block(a, j0, ej0);
	for (k = 0; k < 16; k++)
		tag[k] = (unsigned char)(y[k] ^ ej0[k]);
}

static void
gcm_setup(const unsigned char key[32], const unsigned char nonce[12],
    struct aes256 *a, unsigned char h[16], unsigned char j0[16])
{
	static const unsigned char zero[16] = {0};
	int k;

	aes256_init(a, key);
	aes256_encrypt_block(a, zero, h);
	for (k = 0; k < 12; k++)
		j0[k] = nonce[k];
	j0[12] = 0;
	j0[13] = 0;
	j0[14] = 0;
	j0[15] = 1;
}

void
aes256_gcm_encrypt(const unsigned char key[32],
    const unsigned char nonce[12],
    const unsigned char *aad, unsigned int aad_len,
    const unsigned char *pt, unsigned int len,
    unsigned char *ct, unsigned char tag[16])
{
	struct aes256 a;
	unsigned char h[16], j0[16];

	gcm_setup(key, nonce, &a, h, j0);
	if (len)
		gctr(&a, j0, pt, len, ct);
	gcm_tag(&a, h, j0, aad, aad_len, ct, len, tag);
}

int
aes256_gcm_decrypt(const unsigned char key[32],
    const unsigned char nonce[12],
    const unsigned char *aad, unsigned int aad_len,
    const unsigned char *ct, unsigned int len,
    const unsigned char tag[16], unsigned char *pt)
{
	struct aes256 a;
	unsigned char h[16], j0[16], want[16];
	unsigned char diff = 0;
	int k;

	gcm_setup(key, nonce, &a, h, j0);
	gcm_tag(&a, h, j0, aad, aad_len, ct, len, want);
	for (k = 0; k < 16; k++)
		diff |= (unsigned char)(want[k] ^ tag[k]);
	if (diff)
		return -1;
	if (len)
		gctr(&a, j0, ct, len, pt);
	return 0;
}
