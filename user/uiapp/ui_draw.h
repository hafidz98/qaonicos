/*
 * ui_draw.h - primitif gambar uiapp ke strip framebuffer.
 *
 * Pola sama seperti face_draw: viewport strip (y0, h) 240x24; semua
 * koordinat dalam ruang frame penuh 240x240 dan di-clip ke strip.
 * Warna RGB565.
 */
#ifndef UIAPP_DRAW_H
#define UIAPP_DRAW_H

#include <stdint.h>

#define UI_W	240
#define UI_H	240

/* Palet RGB565 (16-bit!). */
#define C_BG		0x0863		/* latar gelap navy */
#define C_FG		0xDF5F		/* teks terang */
#define C_DIM		0x5B51		/* teks redup */
#define C_ACCENT	0x371F		/* cyan aksen */
#define C_SEL		0x1969		/* latar item terpilih */
#define C_OK		0x3F0F		/* hijau */
#define C_WARN		0xE527		/* kuning */

void	ui_set_strip(uint16_t *buf, int y0, int h);
void	ui_clear(uint16_t color);
void	ui_px(int x, int y, uint16_t c);
void	ui_rect(int x, int y, int w, int h, uint16_t c);
void	ui_border(int x, int y, int w, int h, uint16_t c);
void	ui_text(int x, int y, const char *s, uint16_t c);
void	ui_text_center(int y, const char *s, uint16_t c);
int	ui_text_w(const char *s);

/* Kecil: ubah huruf ke kapital (font hanya punya A-Z). */
void	ui_toupper(char *s);

#endif /* UIAPP_DRAW_H */
