/*
 * uartproto.c - framing protokol UART v1.
 *
 * Baris perintah: "CMD arg..\n".  Respons: "OK payload..\n" atau
 * "ERR kode..\n".  Payload multi-baris (mis. WIFI.SCAN) dikembalikan
 * utuh; pemanggil yang memecah baris.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"

#define TX_SZ	160
#define RX_SZ	512

/* Titik sambung transport. QEMU: mock in-process. HW: UART PL011. */
int	mock_comcu_handle(const char *tx, char *rx, unsigned rsz);

static int
xchg(const char *tx, char *rx, unsigned rsz)
{
	/* QEMU: mock co-MCU. RV1103: ganti dengan tulis/baca PL011 data
	 * + timeout via sys_uptime(). */
	return mock_comcu_handle(tx, rx, rsz);
}

static void
cpycat(char *d, unsigned dsz, const char *a, const char *b, const char *c)
{
	unsigned i = 0, j;
	for (j = 0; a[j] && i + 1 < dsz; j++)
		d[i++] = a[j];
	if (b) {
		if (i + 1 < dsz)
			d[i++] = ' ';
		for (j = 0; b[j] && i + 1 < dsz; j++)
			d[i++] = b[j];
	}
	if (c) {
		if (i + 1 < dsz)
			d[i++] = ' ';
		for (j = 0; c[j] && i + 1 < dsz; j++)
			d[i++] = c[j];
	}
	d[i] = 0;
}

static int
do_cmd(const char *txline, char *resp, unsigned rsz)
{
	char rx[RX_SZ];
	unsigned i, o;

	if (xchg(txline, rx, sizeof(rx)) != 0)
		return -1;
	/* "OK ..." -> payload; "ERR ..." -> -1. */
	if (rx[0] == 'O' && rx[1] == 'K' &&
	    (rx[2] == ' ' || rx[2] == '\n' || rx[2] == 0)) {
		o = 0;
		i = (rx[2] == ' ') ? 3 : 2;
		while (rx[i] && o + 1 < rsz)
			resp[o++] = rx[i++];
		/* buang newline akhir */
		while (o > 0 && (resp[o - 1] == '\n' || resp[o - 1] == '\r'))
			o--;
		resp[o] = 0;
		return 0;
	}
	return -1;
}

int
uproto_cmd(const char *cmd, char *resp, unsigned rsz)
{
	char tx[TX_SZ];
	cpycat(tx, sizeof(tx), cmd, 0, 0);
	return do_cmd(tx, resp, rsz);
}

int
uproto_cmd1(const char *cmd, const char *arg, char *resp, unsigned rsz)
{
	char tx[TX_SZ];
	cpycat(tx, sizeof(tx), cmd, arg, 0);
	return do_cmd(tx, resp, rsz);
}

int
uproto_cmd2(const char *cmd, const char *a1, const char *a2, char *resp,
	    unsigned rsz)
{
	char tx[TX_SZ];
	cpycat(tx, sizeof(tx), cmd, a1, a2);
	return do_cmd(tx, resp, rsz);
}

/* --- Data plane v2: VOICE.START/STOP + frame AUD --- */

/* Titik sambung frame biner. QEMU: mock in-process. */
int	mock_comcu_aud(const unsigned char *frame, unsigned flen);

unsigned
voice_crc16(const unsigned char *p, unsigned len)
{
	unsigned crc = 0xffffu;
	unsigned i, j;

	for (i = 0u; i < len; i++) {
		crc ^= (unsigned)p[i] << 8;
		for (j = 0u; j < 8u; j++)
			crc = (crc & 0x8000u) ? (crc << 1) ^ 0x1021u :
			    crc << 1;
	}
	return crc & 0xffffu;
}

int
uproto_voice_start(unsigned rate, unsigned bits)
{
	char resp[RX_SZ];
	char a1[16], a2[16];
	unsigned i = 0u, v;

	/* utoa sederhana. */
	v = rate;
	do {
		a1[i++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v && i + 1 < sizeof(a1));
	a1[i] = 0;
	/* reverse */
	{
		unsigned j;
		for (j = 0u; j < i / 2u; j++) {
			char t = a1[j];
			a1[j] = a1[i - 1u - j];
			a1[i - 1u - j] = t;
		}
	}
	i = 0u;
	v = bits;
	do {
		a2[i++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v && i + 1 < sizeof(a2));
	a2[i] = 0;
	{
		unsigned j;
		for (j = 0u; j < i / 2u; j++) {
			char t = a2[j];
			a2[j] = a2[i - 1u - j];
			a2[i - 1u - j] = t;
		}
	}
	if (uproto_cmd2("VOICE.START", a1, a2, resp, sizeof(resp)) != 0)
		return -1;
	return 0;
}

int
uproto_voice_stop(void)
{
	char resp[RX_SZ];
	unsigned n = 0u, i;

	if (uproto_cmd("VOICE.STOP", resp, sizeof(resp)) != 0)
		return -1;
	for (i = 0u; resp[i] >= '0' && resp[i] <= '9'; i++)
		n = n * 10u + (unsigned)(resp[i] - '0');
	return (int)n;
}

int
uproto_aud_frame(const unsigned char *pcm, unsigned len)
{
	/* QEMU: frame dirakit lalu diserahkan ke mock in-process.
	 * HW: tulis byte-per-byte ke UART PL011 (titik sambung). */
	static unsigned char frame[VOICE_FRAME_HDR + VOICE_MAX_PAYLOAD +
	    VOICE_FRAME_CRC];
	unsigned crc, flen, i;

	if (len == 0u || len > VOICE_MAX_PAYLOAD)
		return -1;
	frame[0] = VOICE_FRAME_MAGIC0;
	frame[1] = VOICE_FRAME_MAGIC1;
	frame[2] = VOICE_FRAME_MAGIC2;
	frame[3] = (unsigned char)((len >> 8) & 0xffu);
	frame[4] = (unsigned char)(len & 0xffu);
	for (i = 0u; i < len; i++)
		frame[VOICE_FRAME_HDR + i] = pcm[i];
	crc = voice_crc16(frame, VOICE_FRAME_HDR + len);
	frame[VOICE_FRAME_HDR + len] = (unsigned char)((crc >> 8) & 0xffu);
	frame[VOICE_FRAME_HDR + len + 1u] = (unsigned char)(crc & 0xffu);
	flen = VOICE_FRAME_HDR + len + VOICE_FRAME_CRC;
	return mock_comcu_aud(frame, flen);
}
