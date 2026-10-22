/*
 * ui_draw.c - implementasi primitif gambar uiapp.
 */
#include "ui_draw.h"
#include "../face/font5x7.h"

static uint16_t	*strip_buf;
static int	strip_y0, strip_h;

void
ui_set_strip(uint16_t *buf, int y0, int h)
{
	strip_buf = buf;
	strip_y0 = y0;
	strip_h = h;
}

void
ui_clear(uint16_t color)
{
	int i;
	for (i = 0; i < UI_W * strip_h; i++)
		strip_buf[i] = color;
}

void
ui_px(int x, int y, uint16_t c)
{
	int sy;
	if (x < 0 || x >= UI_W || y < 0 || y >= UI_H)
		return;
	sy = y - strip_y0;
	if (sy < 0 || sy >= strip_h)
		return;
	strip_buf[sy * UI_W + x] = c;
}

void
ui_rect(int x, int y, int w, int h, uint16_t c)
{
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			ui_px(x + i, y + j, c);
}

void
ui_border(int x, int y, int w, int h, uint16_t c)
{
	int i;
	for (i = 0; i < w; i++) {
		ui_px(x + i, y, c);
		ui_px(x + i, y + h - 1, c);
	}
	for (i = 0; i < h; i++) {
		ui_px(x, y + i, c);
		ui_px(x + w - 1, y + i, c);
	}
}

static const font5x7_glyph_t *
glyph_for(char ch)
{
	unsigned i;
	for (i = 0; i < sizeof(font5x7) / sizeof(font5x7[0]); i++)
		if (font5x7[i].ch == ch)
			return &font5x7[i];
	return &font5x7[0];	/* ' ' */
}

static void
ui_char(int x, int y, char ch, uint16_t c)
{
	const font5x7_glyph_t *g = glyph_for(ch);
	int col, row;
	for (col = 0; col < 5; col++)
		for (row = 0; row < 7; row++)
			if ((g->col[col] >> row) & 1)
				ui_px(x + col, y + row, c);
}

void
ui_text(int x, int y, const char *s, uint16_t c)
{
	while (*s) {
		ui_char(x, y, *s, c);
		x += 6;
		s++;
	}
}

int
ui_text_w(const char *s)
{
	int n = 0;
	while (*s++) n++;
	return n * 6 - 1;
}

void
ui_text_center(int y, const char *s, uint16_t c)
{
	int w = ui_text_w(s);
	ui_text((UI_W - w) / 2, y, s, c);
}

void
ui_toupper(char *s)
{
	while (*s) {
		if (*s >= 'a' && *s <= 'z')
			*s -= (char)32;
		s++;
	}
}

/* Bresenham line. */
void
ui_line(int x0, int y0, int x1, int y1, uint16_t c)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int sx = x0 < x1 ? 1 : -1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0;	/* -|dy| */
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		int e2;
		ui_px(x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static void
round_row(int x, int y, int w, int inset, uint16_t c)
{
	ui_rect(x + inset, y, w - 2 * inset, 1, c);
}

/* Filled rounded rect (corner radius r <= min(w,h)/2). */
void
ui_round_rect(int x, int y, int w, int h, int r, uint16_t c)
{
	int j;
	for (j = 0; j < h; j++) {
		int inset = 0;
		if (j < r)
			inset = r - j;
		else if (j >= h - r)
			inset = j - (h - r - 1);
		/* soften the corner curve a step */
		if (inset > 0)
			inset = inset * 2 / 3;
		round_row(x, y + j, w, inset, c);
	}
}

/* Rounded rect outline, 1px. */
void
ui_round_border(int x, int y, int w, int h, int r, uint16_t c)
{
	int i;
	/* top/bottom edges with corner arcs (quarter circles
	 * approximated by stepping insets) */
	for (i = 0; i < r; i++) {
		int inset = (r - i) * 2 / 3;
		ui_px(x + inset, y + i, c);
		ui_px(x + w - 1 - inset, y + i, c);
		ui_px(x + inset, y + h - 1 - i, c);
		ui_px(x + w - 1 - inset, y + h - 1 - i, c);
	}
	ui_rect(x + r, y, w - 2 * r, 1, c);
	ui_rect(x + r, y + h - 1, w - 2 * r, 1, c);
	ui_rect(x, y + r, 1, h - 2 * r, c);
	ui_rect(x + w - 1, y + r, 1, h - 2 * r, c);
}
