/*
 * user/uaudiogain.c -- D3: mic gain + overrun counter test.
 *
 * One-shot: set gain levels 0/1/2, verify ANA_CTL2 via readback;
 * invalid level must fail; overrun counter readable.
 * Prints UAUDIOGAIN: PASS/FAIL.
 */
#include "ulib/ulib.h"

#define	ACODEC_ADC_ANA_CTL2	0x90u

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

__attribute__((section(".text.start")))
void
_start(void)
{
	int r, fails = 0;
	unsigned v;
	/* level -> expected ANA_CTL2 gain bits (L[7:6] | R[5:4]). */
	static const unsigned expect[3] = { 0x50u, 0xf0u, 0xa0u };
	unsigned i;

	u_put("uaudiogain: mic gain + overrun test (D3)\n");
	r = sys_audio_init();
	if (r != 0)
		fails++;

	for (i = 0u; i < 3u; i++) {
		r = sys_audio_gain(i);
		u_put("  gain ");
		puthex(i);
		v = sys_audio_reg(ACODEC_ADC_ANA_CTL2);
		u_put("  ANA_CTL2=");
		puthex(v);
		if (r != 0 || (v & 0xf0u) != expect[i]) {
			u_put("  MISMATCH\n");
			fails++;
		}
	}
	/* Invalid level must fail. */
	r = sys_audio_gain(3u);
	if (r == 0) {
		u_put("  bad level accepted!\n");
		fails++;
	}
	/* Overrun counter readable (0 after fresh init). */
	v = sys_audio_overrun();
	u_put("  overrun=");
	puthex(v);

	if (fails == 0)
		u_put("UAUDIOGAIN: PASS\n");
	else
		u_put("UAUDIOGAIN: FAIL\n");
	sys_exit(fails);
}
