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
