/*
 * user/uvoice.c -- D4: VOICE.START + AUD frame uplink test.
 *
 * One-shot: audio_init -> audio_start -> VOICE.START via UART ->
 * loop { audio_read -> uproto_aud_frame } -> VOICE.STOP ->
 * verify mock frame count == sent -> audio_stop.
 * Prints UVOICE: PASS/FAIL.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"

#define	CHUNK	512u
#define	NCHUNK	8u

static unsigned char chunk[CHUNK];

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
	unsigned i;
	int sent = 0, got;

	u_put("uvoice: VOICE.START + AUD uplink test (D4)\n");

	r = sys_audio_init();
	if (r != 0)
		fails++;
	r = sys_audio_start(16000u, 16u, 1u);
	if (r != 0)
		fails++;

	r = uproto_voice_start(16000u, 16u);
	u_put("  VOICE.START -> ");
	puthex((unsigned)r);
	if (r != 0)
		fails++;

	for (i = 0u; i < NCHUNK; i++) {
		int n = sys_audio_read(chunk, CHUNK, 5000u);
		if (n != (int)CHUNK) {
			u_put("  short audio read\n");
			fails++;
			break;
		}
		r = uproto_aud_frame(chunk, CHUNK);
		if (r != 0) {
			u_put("  aud frame rejected\n");
			fails++;
			break;
		}
		sent++;
	}
	u_put("  frames sent=");
	puthex((unsigned)sent);

	got = uproto_voice_stop();
	u_put("  VOICE.STOP -> ");
	puthex((unsigned)got);
	if (got != sent) {
		u_put("  frame count mismatch!\n");
		fails++;
	}

	sys_audio_stop();

	if (fails == 0)
		u_put("UVOICE: PASS\n");
	else
		u_put("UVOICE: FAIL\n");
	sys_exit(fails);
}
