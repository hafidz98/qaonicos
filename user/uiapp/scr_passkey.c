/*
 * scr_passkey.c - daftar kredensial FIDO2 (via FIDO.LIST co-MCU).
 *
 * v1: hanya LIST (mock: 0).  Registrasi/autentikasi butuh co-MCU fisik
 * + BLE companion (RENCANA-app §10/§12) — fase berikutnya.  Private key
 * tidak pernah lewat UART (hanya metadata + approve/deny).
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"
#include "uartproto/uartproto.h"

static char	ncred[8] = "?";

static void
passkey_enter(void)
{
	char resp[32];
	unsigned i = 0;
	if (uproto_cmd("FIDO.LIST", resp, sizeof(resp)) == 0) {
		while (resp[i] && resp[i] != '\n' && i + 1 < sizeof(ncred)) {
			ncred[i] = resp[i];
			i++;
		}
		ncred[i] = 0;
	} else {
		ncred[0] = '?';
		ncred[1] = 0;
	}
}

static void
passkey_event(int ev)
{
	if (ev == EV_BACK)
		ui_pop();
}

static void
passkey_render(void)
{
	char line[40];
	unsigned a = 0, b = 0;
	ui_text_center(22, "PASSKEY", C_DIM);

	while (ncred[b] && a + 1 < sizeof(line))
		line[a++] = ncred[b++];
	{
		const char *t = " KREDENSIAL";
		while (*t && a + 1 < sizeof(line))
			line[a++] = *t++;
	}
	line[a] = 0;
	ui_text_center(70, line, C_FG);

	ui_text_center(120, "FIDO2 BUTUH", C_WARN);
	ui_text_center(134, "CO-MCU FISIK", C_WARN);
	ui_text_center(148, "+ APLIKASI HP", C_WARN);
	ui_text_center(170, "PRIVATE KEY TAK PERNAH", C_DIM);
	ui_text_center(184, "LEWAT UART", C_DIM);
	ui_text_center(208, "CO-MCU: MOCK (QEMU)", C_DIM);
}

const screen_t scr_passkey = {
	passkey_enter, 0, passkey_event, passkey_render
};
