/*
 * credstore.c -- B5.5 credential store (PLAN-B5 section 4,
 * PLAN-B55.md).
 *
 * On-disk format in littlefs file "cfg-<name>":
 *   "QSEC" | ver=1 | nonce[12] | ciphertext | tag[16]
 * A file without the magic is a legacy plaintext secret
 * (pre-B5.5): credstore_get returns it and re-stores it
 * encrypted (transparent migration).
 *
 * Device key: credstore_device_key() is the single seam. This
 * build derives it from a build-time placeholder, which is
 * OBFUSCATION ONLY (stops casual readout, not a determined
 * attacker with the image). The hardware build derives/wraps
 * a random device key with the OTP key (PLAN-B5 section 4) --
 * that is what answers threat T3 for real.
 */

#include "credstore.h"
#include "aesgcm.h"
#include "updverify.h"

extern unsigned int	arm_timer_ticks(void);
extern int		lfs_port_read(const char *, unsigned char *,
			    unsigned int);
extern int		lfs_port_write(const char *, const unsigned char *,
			    unsigned int);
extern int		bootstate_info(unsigned int *, unsigned int *,
			    unsigned int *, unsigned int *);

#define QSEC_OVERHEAD	(4u + 1u + 12u + 16u)

static unsigned int nonce_ctr;

static void
credstore_device_key(unsigned char key[32])
{
	/* DEV PLACEHOLDER -- see the file header. */
	static const char seed[] = "qaon-credstore-devkey-v1";
	upd_sha256((const unsigned char *)seed,
	    sizeof(seed) - 1u, key);
}

static int
name_to_file(const char *name, char *out)
{
	unsigned int n = 0;

	if (!name || !name[0])
		return -1;
	out[0] = 'c';
	out[1] = 'f';
	out[2] = 'g';
	out[3] = '-';
	while (name[n]) {
		char c = name[n];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-'))
			return -1;
		if (n >= CRED_NAME_MAX)
			return -1;
		out[4u + n] = c;
		n++;
	}
	out[4u + n] = 0;
	return 0;
}

static void
make_nonce(const unsigned char key[32], unsigned char nonce[12])
{
	unsigned char msg[44], dig[32];
	unsigned int bc = 0, x = 0, i, n = 0, t;

	(void)bootstate_info(&bc, &x, &x, &x);
	for (i = 0; i < 32u; i++)
		msg[n++] = key[i];
	t = bc;
	for (i = 0; i < 4u; i++) {
		msg[n++] = (unsigned char)(t & 0xffu);
		t >>= 8;
	}
	t = ++nonce_ctr;
	for (i = 0; i < 4u; i++) {
		msg[n++] = (unsigned char)(t & 0xffu);
		t >>= 8;
	}
	t = arm_timer_ticks() * 10u;
	for (i = 0; i < 4u; i++) {
		msg[n++] = (unsigned char)(t & 0xffu);
		t >>= 8;
	}
	upd_sha256(msg, n, dig);
	for (i = 0; i < 12u; i++)
		nonce[i] = dig[i];
}

int
credstore_set(const char *name, const unsigned char *val,
    unsigned int len)
{
	char file[24];
	static unsigned char blob[QSEC_OVERHEAD + CRED_VAL_MAX];
	unsigned char key[32], nonce[12];
	unsigned int i;

	if (len > CRED_VAL_MAX || name_to_file(name, file) != 0)
		return -1;
	credstore_device_key(key);
	make_nonce(key, nonce);
	blob[0] = 'Q';
	blob[1] = 'S';
	blob[2] = 'E';
	blob[3] = 'C';
	blob[4] = 1;
	for (i = 0; i < 12u; i++)
		blob[5u + i] = nonce[i];
	aes256_gcm_encrypt(key, nonce, (const unsigned char *)0, 0, val, len,
	    blob + 17u, blob + 17u + len);
	return lfs_port_write(file, blob, QSEC_OVERHEAD + len);
}

int
credstore_get(const char *name, unsigned char *buf, unsigned int max)
{
	char file[24];
	static unsigned char raw[QSEC_OVERHEAD + CRED_VAL_MAX];
	int n;
	unsigned int len;

	if (name_to_file(name, file) != 0)
		return -1;
	n = lfs_port_read(file, raw, sizeof(raw));
	if (n < 0)
		return -1;
	if (n >= (int)QSEC_OVERHEAD && raw[0] == 'Q' && raw[1] == 'S' &&
	    raw[2] == 'E' && raw[3] == 'C' && raw[4] == 1) {
		unsigned char key[32];
		len = (unsigned int)n - QSEC_OVERHEAD;
		if (len > max)
			return -1;
		credstore_device_key(key);
		if (aes256_gcm_decrypt(key, raw + 5u, (const unsigned char *)0, 0,
		    raw + 17u, len, raw + 17u + len, buf) != 0)
			return -1;
		return (int)len;
	}
	/* legacy plaintext: return it, then migrate */
	if ((unsigned int)n > max)
		return -1;
	{
		unsigned int i;
		for (i = 0; i < (unsigned int)n; i++)
			buf[i] = raw[i];
		(void)credstore_set(name, buf, (unsigned int)n);
	}
	return n;
}

int
credstore_present(const char *name)
{
	char file[24];
	unsigned char probe[1];

	if (name_to_file(name, file) != 0)
		return 0;
	/* A present secret has >= 1 byte (QSEC blob or legacy
	 * value); a cleared file is empty. */
	return lfs_port_read(file, probe, 1u) > 0;
}

int
credstore_clear(const char *name)
{
	char file[24];

	if (name_to_file(name, file) != 0)
		return -1;
	return lfs_port_write(file, (const unsigned char *)"", 0u);
}

/* --- self-test (SYS_CRED sub 9), vectors from PLAN-B55 --- */

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return 0;
}

static unsigned int
unhex(const char *s, unsigned char *out)
{
	unsigned int n = 0;
	while (s[2u * n]) {
		out[n] = (unsigned char)(hexval(s[2u * n]) * 16 +
		    hexval(s[2u * n + 1u]));
		n++;
	}
	return n;
}

static int
vec_ok(const char *khex, const char *nhex, const char *phex,
    const char *chex, const char *thex)
{
	unsigned char key[32], nonce[12], pt[64], ct[64], tag[16];
	unsigned char want[64], wtag[16], back[64];
	unsigned int pl, i;

	unhex(khex, key);
	unhex(nhex, nonce);
	pl = unhex(phex, pt);
	aes256_gcm_encrypt(key, nonce, (const unsigned char *)0, 0, pt, pl, ct, tag);
	unhex(chex, want);
	unhex(thex, wtag);
	for (i = 0; i < pl; i++)
		if (ct[i] != want[i])
			return 0;
	for (i = 0; i < 16u; i++)
		if (tag[i] != wtag[i])
			return 0;
	if (aes256_gcm_decrypt(key, nonce, (const unsigned char *)0, 0, ct, pl, tag,
	    back) != 0)
		return 0;
	for (i = 0; i < pl; i++)
		if (back[i] != pt[i])
			return 0;
	return 1;
}

int
credstore_selftest(void)
{
	int mask = 0;
	static const char secret[] = "selftest-secret-0123456789";
	unsigned char buf[64];
	int n;

	if (vec_ok(
	    "000102030405060708090a0b0c0d0e0f"
	    "101112131415161718191a1b1c1d1e1f",
	    "000102030405060708090a0b",
	    "51616f6e69634f53206372656473746f7265207631",
	    "1663b975ac868d48ad22e5eed59a0c02f1b3a742c1",
	    "5784815de3f41e9659eafe77891773b2"))
		mask |= 1;
	if (vec_ok(
	    "000102030405060708090a0b0c0d0e0f"
	    "101112131415161718191a1b1c1d1e1f",
	    "ffffffffffffffffffffffff",
	    "",
	    "",
	    "d879e1252052a18ea14f5e3de6ade735"))
		mask |= 2;
	if (vec_ok(
	    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5",
	    "5a5a5a5a5a5a5a5a5a5a5a5a",
	    "54686520717569636b2062726f776e20666f78206a756d7073206f76657220746865206c617a7920646f672030313233343536373839",
	    "fce260318e527cb45f50b987f22c7f238defaf0ce24c816ae377ad86601b3342202ad422a88061223a4438f33692c18bb75249910a9a",
	    "0c54173d7eee0ffac856fc88d175cd6a"))
		mask |= 4;
	/* store roundtrip on a scratch name */
	if (credstore_set("selftest",
	    (const unsigned char *)secret, sizeof(secret) - 1u) == 0) {
		n = credstore_get("selftest", buf, sizeof(buf));
		if (n == (int)(sizeof(secret) - 1u)) {
			unsigned int i;
			int same = 1;
			for (i = 0; i < (unsigned int)n; i++)
				if (buf[i] != (unsigned char)secret[i])
					same = 0;
			if (same)
				mask |= 8;
		}
		(void)credstore_clear("selftest");
	}
	return mask;
}
