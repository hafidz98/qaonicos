/*
 * scr_settings.c - daftar settings.
 *
 * v1 (App A2): WiFi/Bluetooth/LLM/Passkey butuh co-MCU ESP32-C3 via
 * UART (fase berikutnya).  Memilih item menampilkan catatan jujur —
 * tanpa fungsi palsu.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

#define NITEM	4
static const char *items[NITEM] = {
	"WIFI", "BLUETOOTH", "LLM", "PASSKEY"
};
static int	sel = 0;
static int	show_note = 0;

#define Y0	52
#define LH	26

static void
set_enter(void)
{
	sel = 0;
	show_note = 0;
}

static void
set_event(int ev)
{
	if (show_note) {
		/* tombol apa pun menutup catatan */
		show_note = 0;
		return;
	}
	switch (ev) {
	case EV_UP:
		if (sel > 0) sel--;
		break;
	case EV_DOWN:
		if (sel < NITEM - 1) sel++;
		break;
	case EV_OK:
		show_note = 1;
		break;
	case EV_BACK:
		ui_pop();
		break;
	default:
		break;
	}
}

static void
set_render(void)
{
	int i;
	ui_text_center(22, "SETTINGS", C_DIM);
	for (i = 0; i < NITEM; i++) {
		int y = Y0 + i * LH;
		if (i == sel) {
			ui_rect(16, y - 4, UI_W - 32, 20, C_SEL);
			ui_border(16, y - 4, UI_W - 32, 20, C_ACCENT);
		}
		ui_text(28, y, items[i], i == sel ? C_ACCENT : C_FG);
	}
	if (show_note) {
		ui_rect(28, 150, UI_W - 56, 56, 0x08A5);
		ui_border(28, 150, UI_W - 56, 56, C_WARN);
		ui_text_center(162, "BUTUH CO-MCU", C_WARN);
		ui_text_center(176, "ESP32-C3", C_WARN);
	}
}

const screen_t scr_settings = {
	set_enter, 0, set_event, set_render
};
