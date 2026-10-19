/*
 * user/uaudiocap.c -- D2: I2S RX capture test (PIO).
 *
 * One-shot: audio_init -> audio_start(16000,16,1) -> read N bytes
 * via sys_audio_read -> verify incrementing pattern (QEMU virtual
 * RXDR) -> audio_stop. Prints UAUDIOCAP: PASS/FAIL.
 */
#include "ulib/ulib.h"

#define	CAP_BYTES	4096u

static unsigned char capbuf[CAP_BYTES];

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
	int r, n;
	unsigned i;
	unsigned short expect = 0u;
	int fails = 0;

	u_put("uaudiocap: I2S RX capture test (D2, PIO)\n");

	r = sys_audio_init();
	u_put("  audio_init -> ");
	puthex((unsigned)r);
	if (r != 0)
		fails++;

	r = sys_audio_start(16000u, 16u, 1u);
	u_put("  audio_start -> ");
	puthex((unsigned)r);
	if (r != 0)
		fails++;

	/* Invalid params must fail. */
	r = sys_audio_start(44100u, 16u, 1u);
	if (r == 0) {
		u_put("  bad rate accepted!\n");
		fails++;
	}
	r = sys_audio_start(16000u, 8u, 1u);
	if (r == 0) {
		u_put("  bad bits accepted!\n");
		fails++;
	}

	/* Capture. QEMU virtual RXDR = incrementing 16-bit counter. */
	n = sys_audio_read(capbuf, CAP_BYTES, 5000u);
	u_put("  audio_read -> ");
	puthex((unsigned)n);
	if (n != (int)CAP_BYTES) {
		u_put("  short read!\n");
		fails++;
	} else {
		/* QEMU: L samples are even (R skipped: ctr+=2 per frame). */
		for (i = 0u; i + 1u < CAP_BYTES; i += 2u) {
			unsigned short s =
			    (unsigned short)capbuf[i] |
			    ((unsigned short)capbuf[i + 1u] << 8);
			if (s != expect) {
				u_put("  pattern mismatch at ");
				puthex(i);
				u_put("  got=");
				puthex(s);
				u_put("  expect=");
				puthex(expect);
				fails++;
				break;
			}
			expect += 2u;
		}
	}

	r = sys_audio_stop();
	u_put("  audio_stop -> ");
	puthex((unsigned)r);

	if (fails == 0)
		u_put("UAUDIOCAP: PASS\n");
	else
		u_put("UAUDIOCAP: FAIL\n");
	sys_exit(fails);
}
