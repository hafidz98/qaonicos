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
