/* Primitive drawing into the RGB565 framebuffer. All clipping-safe. */
#ifndef FACE_DRAW_H
#define FACE_DRAW_H

#include <stdint.h>

#define FB_W 240
#define FB_H 240

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Scale a color's brightness by 0..255. */
static inline uint16_t dim565(uint16_t c, uint8_t k) {
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (r * k) >> 8; g = (g * k) >> 8; b = (b * k) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void fb_clear(uint16_t color);
/* Viewport untuk strip rendering (App A1). Default = full frame. */
void fb_set_view(int y0, int h);
void fb_reset_view(void);
void fb_clear_view(uint16_t color);
void fb_pixel(int x, int y, uint16_t color);
void fb_fill_rect(int x, int y, int w, int h, uint16_t color);
/* Filled rounded rect (squircle-ish). r clamped to min(w,h)/2. */
void fb_round_rect(int x, int y, int w, int h, int r, uint16_t color);
/* Filled rounded rect rotated by deg (clockwise) around its center. */
void fb_round_rect_rot(int cx, int cy, int w, int h, int r, int deg,
                       uint16_t color);
void fb_fill_circle(int cx, int cy, int rad, uint16_t color);
/* Circle outline, thickness px. */
void fb_ring(int cx, int cy, int rad, int thick, uint16_t color);
/* Filled polar blob: r(theta) = R*(1 + a1*sin(3t+p1) + a2*sin(5t+p2)). */
void fb_blob(int cx, int cy, int R, int a1, int p1, int a2, int p2,
             uint16_t color);
/* Waveform bars across [x0,x1] at baseline y. amp 0..64, phase 0..255. */
void fb_waveform(int x0, int x1, int y, int amp, uint8_t phase,
                 uint16_t color);
/* 5x7 text, 1px spacing, top-left at (x,y). */
void fb_text(int x, int y, const char *s, uint16_t color);
int fb_text_width(const char *s);

#endif /* FACE_DRAW_H */
