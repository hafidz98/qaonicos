/*
 * scr_settings.c - daftar settings -> layar WiFi/BLE/LLM/Passkey.
 *
 * Tiap layar bicara ke co-MCU via protokol UART v1 (mock di QEMU).
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

extern const screen_t scr_wifi;
extern const screen_t scr_ble;
extern const screen_t scr_llm;
extern const screen_t scr_passkey;

#define NITEM	4
static const char *items[NITEM] = {
	"WIFI", "BLUETOOTH", "LLM", "PASSKEY"
};
static const screen_t *targets[NITEM];
static int	sel = 0;

#define Y0	52
#define LH	30

static void
set_enter(void)
{
	targets[0] = &scr_wifi;
	targets[1] = &scr_ble;
	targets[2] = &scr_llm;
	targets[3] = &scr_passkey;
	sel = 0;
}

static void
set_event(int ev)
{
	switch (ev) {
	case EV_UP:
		if (sel > 0)
			sel--;
		break;
	case EV_DOWN:
		if (sel < NITEM - 1)
			sel++;
		break;
	case EV_OK:
		ui_push(targets[sel]);
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
			ui_rect(16, y - 4, UI_W - 32, 24, C_SEL);
			ui_border(16, y - 4, UI_W - 32, 24, C_ACCENT);
		}
		ui_text(28, y, items[i], i == sel ? C_ACCENT : C_FG);
		ui_text(UI_W - 40, y, ">", C_DIM);
	}
	ui_text_center(208, "CO-MCU: MOCK (QEMU)", C_DIM);
}

const screen_t scr_settings = {
	set_enter, 0, set_event, set_render
};
