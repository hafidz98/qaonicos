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
#define GRID_Y0		40
#define CELL_W		100
#define CELL_H		82
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

/* Midpoint circle outline (integer math only). With
 * top_gap > 0, points within top_gap px of the top centre
 * are skipped (power symbol opening). */
static void
draw_circle_g(int cx, int cy, int r, uint16_t c, int top_gap)
{
	int x = r, y = 0, err = 0;
	while (x >= y) {
		int pts[8][2], i;
		pts[0][0] = x;  pts[0][1] = y;
		pts[1][0] = x;  pts[1][1] = -y;
		pts[2][0] = -x; pts[2][1] = y;
		pts[3][0] = -x; pts[3][1] = -y;
		pts[4][0] = y;  pts[4][1] = x;
		pts[5][0] = y;  pts[5][1] = -x;
		pts[6][0] = -y; pts[6][1] = x;
		pts[7][0] = -y; pts[7][1] = -x;
		for (i = 0; i < 8; i++) {
			if (top_gap > 0 && pts[i][1] <= -r + 3 &&
			    pts[i][0] >= -top_gap && pts[i][0] <= top_gap)
				continue;
			ui_px(cx + pts[i][0], cy + pts[i][1], c);
		}
		y++;
		err += 1 + 2 * y;
		if (2 * (err - x) + 1 > 0) {
			x--;
			err += 1 - 2 * x;
		}
	}
}

static void
draw_circle(int cx, int cy, int r, uint16_t c)
{
	draw_circle_g(cx, cy, r, c, 0);
}

/* Line icons in ONE consistent family (mock review
 * 2026-10-22): thin outline glyphs, ~2px stroke, no fills
 * except the Qabot eyes. Canvas: tile centre (cx, cy). */
static void
draw_icon(int x, int y, int idx, uint16_t c, uint16_t bg)
{
	int cx = x + 48, cy = y + 27;
	(void)bg;
	switch (idx) {
	case 0:	/* Qabot: squircle outline + eyes */
		ui_round_border(cx - 14, cy - 14, 28, 28, 9, c);
		ui_round_border(cx - 13, cy - 13, 26, 26, 8, c);
		ui_rect(cx - 8, cy - 4, 4, 9, c);
		ui_rect(cx + 4, cy - 4, 4, 9, c);
		break;
	case 1:	/* Monitor: screen outline + chart line */
		ui_round_border(cx - 16, cy - 13, 32, 23, 3, c);
		ui_round_border(cx - 15, cy - 12, 30, 21, 3, c);
		ui_line(cx - 11, cy + 4, cx - 4, cy - 3, c);
		ui_line(cx - 4, cy - 3, cx + 2, cy + 0, c);
		ui_line(cx + 2, cy + 0, cx + 10, cy - 7, c);
		ui_line(cx, cy + 10, cx, cy + 13, c);
		ui_line(cx - 6, cy + 13, cx + 6, cy + 13, c);
		break;
	case 2:	/* Settings: gear ring + teeth + hub */
		draw_circle(cx, cy, 10, c);
		draw_circle(cx, cy, 11, c);
		ui_rect(cx - 1, cy - 16, 3, 5, c);
		ui_rect(cx - 1, cy + 11, 3, 5, c);
		ui_rect(cx - 16, cy - 1, 5, 3, c);
		ui_rect(cx + 11, cy - 1, 5, 3, c);
		ui_line(cx + 8, cy - 11, cx + 11, cy - 14, c);
		ui_line(cx - 11, cy + 11, cx - 8, cy + 8, c);
		ui_line(cx - 11, cy - 14, cx - 8, cy - 11, c);
		ui_line(cx + 8, cy + 8, cx + 11, cy + 11, c);
		draw_circle(cx, cy, 3, c);
		break;
	case 3:	/* Power: ring with top gap + stem */
		draw_circle_g(cx, cy + 1, 11, c, 3);
		draw_circle_g(cx, cy + 1, 10, c, 3);
		ui_rect(cx - 1, cy - 13, 3, 11, c);
		break;
	default:
		break;
	}
	(void)GAP;
}

/* Mock-style tiles (owner review 2026-10-22): rounded
 * cards, line icons, labels underneath, no "MENU" title
 * text -- the status bar already frames the screen. */
static void
menu_render(void)
{
	int i;
	for (i = 0; i < NITEM; i++) {
		int col = i % 2, row = i / 2;
		int x = GRID_X0 + col * CELL_W;
		int y = GRID_Y0 + row * CELL_H;
		int is_sel = (i == sel);
		if (is_sel) {
			ui_round_rect(x, y, CELL_W - 4, CELL_H - 6, 10,
			    C_SEL);
			ui_round_border(x, y, CELL_W - 4, CELL_H - 6,
			    10, C_ACCENT);
			ui_round_border(x + 1, y + 1, CELL_W - 6,
			    CELL_H - 8, 9, C_ACCENT);
		} else {
			ui_round_border(x, y, CELL_W - 4, CELL_H - 6,
			    10, C_DIM);
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
