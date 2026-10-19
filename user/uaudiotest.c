/*
 * user/uaudiotest.c -- D1: ACODEC init + register readback test.
 *
 * One-shot (pola utimeset.c): sys_audio_init() lalu baca-balik
 * register kunci; verifikasi masked (agar lolos juga di silikon
 * asli yang reset value-nya belum tentu nol).
 * Prints UAUDIO: PASS/FAIL.
 */
#include "ulib/ulib.h"

#define	ACODEC_ADC_ANA_CTL0	0x88u
#define	ACODEC_ADC_ANA_CTL1	0x8cu
#define	ACODEC_ADC_ANA_CTL2	0x90u
#define	ACODEC_ADC_ANA_CTL3	0x94u
#define	ACODEC_ADC_ANA_CTL6	0xa0u

static void
puthex(unsigned v)
{
	static const char *h = "0123456789abcdef";
	char b[11];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 8; i++)
		b[2 + i] = h[(v >> (28 - i * 4)) & 0xFu];
	b[10] = '\n';
	sys_write(1, b, 11u);
}

static int
check(unsigned off, unsigned mask, unsigned expect, const char *name)
{
	unsigned v = sys_audio_reg(off);

	u_put("  ");
	u_put(name);
	u_put("=");
	puthex(v);
	if ((v & mask) != expect) {
		u_put("  MISMATCH mask=");
		puthex(mask);
		u_put("  expect=");
		puthex(expect);
		return 1;
	}
	return 0;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int fails = 0;
	int r;

	u_put("uaudiotest: ACODEC init + readback (D1)\n");
	r = sys_audio_init();
	u_put("  audio_init -> ");
	puthex((unsigned)r);
	if (r != 0)
		fails++;

	/* Masked checks (driver bit defs, rv1106_codec.h). */
	fails += check(ACODEC_ADC_ANA_CTL0, 0x10u, 0x10u, "ANA_CTL0");
	fails += check(ACODEC_ADC_ANA_CTL1, 0xeeu, 0xeeu, "ANA_CTL1");
	fails += check(ACODEC_ADC_ANA_CTL2, 0xa0u, 0xa0u, "ANA_CTL2");
	fails += check(ACODEC_ADC_ANA_CTL3, 0x33u, 0x33u, "ANA_CTL3");
	fails += check(ACODEC_ADC_ANA_CTL6, 0xffu, 0xffu, "ANA_CTL6");

	if (fails == 0)
		u_put("UAUDIO: PASS\n");
	else
		u_put("UAUDIO: FAIL\n");
	sys_exit(fails);
}
