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

/* Midpoint circle outline (integer math only). */
static void
draw_circle(int cx, int cy, int r, uint16_t c)
{
	int x = r, y = 0, err = 0;
	while (x >= y) {
		ui_px(cx + x, cy + y, c); ui_px(cx + y, cy + x, c);
		ui_px(cx - y, cy + x, c); ui_px(cx - x, cy + y, c);
		ui_px(cx - x, cy - y, c); ui_px(cx - y, cy - x, c);
		ui_px(cx + y, cy - x, c); ui_px(cx + x, cy - y, c);
		y++;
		err += 1 + 2 * y;
		if (2 * (err - x) + 1 > 0) {
			x--;
			err += 1 - 2 * x;
		}
	}
}

/* Rounded eye (Qabot tile): filled rect with cut corners. */
static void
draw_eye_glyph(int x, int y, int w, int h, uint16_t c)
{
	ui_rect(x + 1, y, w - 2, h, c);
	ui_rect(x, y + 1, w, h - 2, c);
}

/* Per-item glyphs, free-standing (no box frame -- the framed
 * look read as raw wireframe; owner feedback 2026-10-22).
 * Canvas: tile centre (cx, cy), glyphs ~36px. */
static void
draw_icon(int x, int y, int idx, uint16_t c, uint16_t bg)
{
	int cx = x + 48, cy = y + 26;
	switch (idx) {
	case 0:	/* Qabot: the face eyes */
		draw_eye_glyph(cx - 19, cy - 10, 13, 20, c);
		draw_eye_glyph(cx + 6, cy - 10, 13, 20, c);
		break;
	case 1:	/* Monitor: ascending bars + axis */
		ui_rect(cx - 16, cy - 2, 7, 12, c);
		ui_rect(cx - 5, cy - 8, 7, 18, c);
		ui_rect(cx + 6, cy - 14, 7, 24, c);
		ui_rect(cx - 18, cy + 11, 38, 2, c);
		break;
	case 2:	/* Settings: gear (ring + 4 teeth + hub hole) */
		draw_circle(cx, cy, 10, c);
		draw_circle(cx, cy, 9, c);
		ui_rect(cx - 2, cy - 14, 5, 5, c);
		ui_rect(cx - 2, cy + 9, 5, 5, c);
		ui_rect(cx - 14, cy - 2, 5, 5, c);
		ui_rect(cx + 9, cy - 2, 5, 5, c);
		ui_rect(cx - 3, cy - 3, 7, 7, bg);
		break;
	case 3:	/* Power: ring with top gap + stem */
		draw_circle(cx, cy + 1, 11, c);
		ui_rect(cx - 3, cy - 12, 7, 4, bg);
		ui_rect(cx - 1, cy - 13, 3, 11, c);
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
		draw_icon(x, y, i, is_sel ? C_ACCENT : C_FG,
		    is_sel ? C_SEL : C_BG);
		/* Label centered WITHIN the tile (ui_text_center
		 * centers on the whole screen -- with two tiles
		 * per row the labels overlapped into mush). */
		ui_text(x + (CELL_W - 4 - ui_text_w(labels[i])) / 2,
		    y + CELL_H - 22, labels[i],
		    is_sel ? C_ACCENT : C_DIM);
	}
}

const screen_t scr_menu = {
	menu_enter, menu_exit, menu_event, menu_render
};
