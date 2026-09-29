/*
 * scr_ble.c - layar Bluetooth via protokol UART v1 (co-MCU mock).
 *
 * BLE.ON / BLE.OFF untuk toggle, BLE.STATUS untuk baca status.
 * Provisioning utama dari HP (RENCANA-app §3) — layar ini kontrol dasar.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"
#include "uartproto/uartproto.h"

static char	ble_state[8] = "?";
static char	ble_name[24] = "?";
static int	sel = 0;
static int	note_kind = 0;
static char	note_txt[48] = "";

static void
set_note(int kind, const char *t)
{
	unsigned i = 0;
	note_kind = kind;
	while (t[i] && i + 1 < sizeof(note_txt)) {
		note_txt[i] = t[i];
		i++;
	}
	note_txt[i] = 0;
}

static void
refresh(void)
{
	char resp[64];
	unsigned i = 0, o = 0;
	if (uproto_cmd("BLE.STATUS", resp, sizeof(resp)) != 0) {
		set_note(2, "CO-MCU TAK RESPON");
		return;
	}
	/* "ON <nama>" / "OFF <nama>" */
	while (resp[o] && resp[o] != ' ' && o + 1 < sizeof(ble_state)) {
		ble_state[o] = resp[o];
		o++;
	}
	ble_state[o] = 0;
	if (resp[o] == ' ')
		o++;
	while (resp[o] && i + 1 < sizeof(ble_name))
		ble_name[i++] = resp[o++];
	ble_name[i] = 0;
}

static void
ble_enter(void)
{
	sel = 0;
	note_kind = 0;
	refresh();
}

static void
ble_event(int ev)
{
	char resp[64];
	if (note_kind) {
		note_kind = 0;
		return;
	}
	switch (ev) {
	case EV_UP:
		if (sel > 0)
			sel--;
		break;
	case EV_DOWN:
		if (sel < 1)
			sel++;
		break;
	case EV_OK:
		if (sel == 0) {
			int turn_on = !(ble_state[0] == 'O' &&
					ble_state[1] == 'N');
			if (uproto_cmd(turn_on ? "BLE.ON" : "BLE.OFF",
				       resp, sizeof(resp)) == 0) {
				set_note(1, turn_on ? "BLE NYALA" :
						     "BLE MATI");
				refresh();
			} else {
				set_note(2, "GAGAL");
			}
		}
		break;
	case EV_BACK:
		ui_pop();
		break;
	default:
		break;
	}
}

static void
ble_render(void)
{
	int i, y;
	char line[48];
	unsigned a = 0, b = 0;
	ui_text_center(22, "BLUETOOTH", C_DIM);

	ui_text_center(44, ble_state, ble_state[0] == 'O' &&
		     ble_state[1] == 'N' ? C_OK : C_DIM);
	while (ble_name[b] && a + 1 < sizeof(line))
		line[a++] = ble_name[b++];
	line[a] = 0;
	ui_text_center(62, line, C_FG);

	{
		const char *rows[2] = { "NYALA/MATI", "NAMA: QABOT-01" };
		for (i = 0; i < 2; i++) {
			y = 100 + i * 30;
			if (i == sel) {
				ui_rect(24, y - 4, UI_W - 48, 22, C_SEL);
				ui_border(24, y - 4, UI_W - 48, 22, C_ACCENT);
			}
			ui_text_center(y, rows[i],
				       i == sel ? C_ACCENT : C_FG);
		}
	}
	ui_text_center(180, "PROVISIONING UTAMA", C_DIM);
	ui_text_center(194, "VIA HP (BLE GATT)", C_DIM);
	ui_text_center(208, "CO-MCU: MOCK (QEMU)", C_DIM);

	if (note_kind) {
		ui_rect(28, 140, UI_W - 56, 56, 0x08A5);
		ui_border(28, 140, UI_W - 56, 56,
			  note_kind == 2 ? C_WARN : C_OK);
		ui_text_center(162, note_txt,
			       note_kind == 2 ? C_WARN : C_OK);
	}
	(void)y;
}

const screen_t scr_ble = {
	ble_enter, 0, ble_event, ble_render
};
