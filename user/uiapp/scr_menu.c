/*
 * scr_menu.c - main menu uiapp: icon grid 2 kolom.
 *
 * Model navigasi §2.3: select dulu (panah), baru masuk (OK).
 * BACK di root -> kembali ke Qabot (lepas token).
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

extern const screen_t scr_mon;
extern const screen_t scr_settings;
extern const screen_t scr_power;

#define NITEM	4
static const char *labels[NITEM] = { "QABOT", "MONITOR", "SETTINGS", "POWER" };
static int	sel = 0;

#define GRID_X0		20
#define GRID_Y0		44
#define CELL_W		100
#define CELL_H		76
#define GAP		0

static void
menu_enter(void)
{
	sel = 0;
}

static void
menu_exit(void)
{
}

static void
menu_event(int ev)
{
	int col, row;
	switch (ev) {
	case EV_UP:
		if (sel >= 2) sel -= 2;
		break;
	case EV_DOWN:
		if (sel < 2) sel += 2;
		break;
	case EV_LEFT:
		col = sel % 2;
		if (col > 0) sel--;
		break;
	case EV_RIGHT:
		col = sel % 2;
		if (col < 1) sel++;
		break;
	case EV_OK:
		if (sel == 0)
			ui_request_release();	/* Qabot = kembali */
		else if (sel == 1)
			ui_push(&scr_mon);
		else if (sel == 2)
			ui_push(&scr_settings);
		else
			ui_push(&scr_power);
		break;
	case EV_BACK:
		ui_request_release();
		break;
	default:
		break;
	}
	(void)col; (void)row;
}

/* Glyph sederhana per item (kotak + simbol). */
static void
draw_icon(int x, int y, int idx, uint16_t c)
{
	int cx = x + 34, cy = y + 22;
	ui_border(x + 22, y + 8, 56, 40, c);
	switch (idx) {
	case 0:	/* Qabot: dua mata */
		ui_rect(cx - 14, cy - 4, 12, 14, c);
		ui_rect(cx + 2, cy - 4, 12, 14, c);
		break;
	case 1:	/* Monitor: bar */
		ui_rect(cx - 12, cy + 6, 6, 8, c);
		ui_rect(cx - 4, cy, 6, 14, c);
		ui_rect(cx + 4, cy - 6, 6, 20, c);
		break;
	case 2:	/* Settings: gear ~ kotak + lubang */
		ui_rect(cx - 10, cy - 10, 20, 20, c);
		ui_rect(cx - 5, cy - 5, 10, 10, C_BG);
		break;
	case 3:	/* Power: lingkaran + garis */
		ui_border(cx - 9, cy - 7, 18, 18, c);
		ui_rect(cx - 1, cy - 12, 3, 10, c);
		break;
	default:
		break;
	}
	(void)GAP;
}

static void
menu_render(void)
{
	int i;
	ui_text_center(22, "MENU", C_DIM);
	for (i = 0; i < NITEM; i++) {
		int col = i % 2, row = i / 2;
		int x = GRID_X0 + col * CELL_W;
		int y = GRID_Y0 + row * CELL_H;
		int is_sel = (i == sel);
		if (is_sel) {
			ui_rect(x, y, CELL_W - 4, CELL_H - 4, C_SEL);
			ui_border(x, y, CELL_W - 4, CELL_H - 4, C_ACCENT);
		} else {
			ui_border(x, y, CELL_W - 4, CELL_H - 4, C_DIM);
		}
		draw_icon(x, y, i, is_sel ? C_ACCENT : C_FG);
		ui_text_center(y + CELL_H - 22, labels[i],
			       is_sel ? C_ACCENT : C_DIM);
	}
}

const screen_t scr_menu = {
	menu_enter, menu_exit, menu_event, menu_render
};
