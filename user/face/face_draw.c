#include "face_draw.h"
#include "face.h"   /* face_fb */
#include "font5x7.h"

/* ---- fixed point sin: input 0..255 -> -128..127 (sin of angle*2pi/256) ---- */
static int fsin(uint8_t a) {
    /* quarter table 0..64 */
    static const uint8_t q[65] = {
        0,3,6,9,13,16,19,22,25,28,31,34,37,40,43,46,49,52,55,58,60,63,66,68,
        71,74,76,79,81,84,86,88,91,93,95,97,99,101,103,105,106,108,110,111,
        113,114,116,117,118,119,121,122,122,123,124,125,126,126,127,127,127,
        128,128,128,128
    };
    uint8_t i = a & 0x3F;
    uint8_t quad = a >> 6;
    /* sin(quad*90° + t): q0=sin t, q1=cos t=sin(90°-t), q2=-sin t, q3=-cos t */
    if (quad == 0) return (int)q[i];
    if (quad == 1) return (int)q[64 - i];
    if (quad == 2) return -(int)q[i];
    return -(int)q[64 - i];
}
/* cos via sin */
#define fcos(a) fsin((uint8_t)((a) + 64))

void fb_clear(uint16_t color) {
    uint32_t n = (uint32_t)FB_W * FB_H;
    for (uint32_t i = 0; i < n; i++) face_fb[i] = color;
}

/* ---- viewport (App A1: strip rendering ke framebuffer parsial) ----
 * Default = seluruh frame. fb_set_view membatasi area logis yang
 * digambar; face_fb menunjuk ke awal area tersebut (strip buffer). */
static int vw_y0 = 0, vw_h = FB_H;

void fb_set_view(int y0, int h) {
    if (y0 < 0) { h += y0; y0 = 0; }
    if (y0 + h > FB_H) h = FB_H - y0;
    if (h < 0) h = 0;
    vw_y0 = y0;
    vw_h = h;
}

void fb_reset_view(void) {
    vw_y0 = 0;
    vw_h = FB_H;
}

void fb_clear_view(uint16_t color) {
    uint32_t n = (uint32_t)FB_W * (uint32_t)vw_h;
    for (uint32_t i = 0; i < n; i++) face_fb[i] = color;
}

void fb_pixel(int x, int y, uint16_t color) {
    int yy = y - vw_y0;
    if ((unsigned)x < (unsigned)FB_W && (unsigned)yy < (unsigned)vw_h)
        face_fb[yy * FB_W + x] = color;
}

void fb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    int y1 = vw_y0 + vw_h;
    if (x < 0) { w += x; x = 0; }
    if (y < vw_y0) { h -= (vw_y0 - y); y = vw_y0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > y1) h = y1 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint16_t *row = &face_fb[(y - vw_y0 + j) * FB_W + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

/* point-in-rounded-rect test, rect centered at (0,0), half sizes hw,hh */
static int in_rrect(int px, int py, int hw, int hh, int r) {
    int ax = px < 0 ? -px : px, ay = py < 0 ? -py : py;
    if (ax > hw || ay > hh) return 0;
    int cx = ax - (hw - r), cy = ay - (hh - r);
    if (cx <= 0 || cy <= 0) return 1;
    return cx * cx + cy * cy <= r * r;
}

void fb_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int hw = w / 2, hh = h / 2, cx = x + hw, cy = y + hh;
    for (int j = -hh; j <= hh; j++)
        for (int i = -hw; i <= hw; i++)
            if (in_rrect(i, j, hw, hh, r))
                fb_pixel(cx + i, cy + j, color);
}

/* rotate (i,j) by -deg (inverse), deg in degrees clockwise */
void fb_round_rect_rot(int cx, int cy, int w, int h, int r, int deg,
                       uint16_t color) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int hw = w / 2, hh = h / 2;
    /* bounding radius */
    int br = hw > hh ? hw : hh;
    /* deg -> 0..255 angle; use coarse rotation via sin/cos */
    uint8_t a = (uint8_t)((deg * 256) / 360);
    int s = fsin(a), c = fcos(a);
    for (int j = -br; j <= br; j++) {
        for (int i = -br; i <= br; i++) {
            /* inverse rotate by -deg: [c s; -s c] * (i,j) / 128 */
            int u = (c * i + s * j) / 128;
            int v = (-s * i + c * j) / 128;
            if (in_rrect(u, v, hw, hh, r))
                fb_pixel(cx + i, cy + j, color);
        }
    }
}

void fb_fill_circle(int cx, int cy, int rad, uint16_t color) {
    for (int j = -rad; j <= rad; j++)
        for (int i = -rad; i <= rad; i++)
            if (i * i + j * j <= rad * rad)
                fb_pixel(cx + i, cy + j, color);
}

void fb_ring(int cx, int cy, int rad, int thick, uint16_t color) {
    int r0 = rad - thick / 2, r1 = rad + thick / 2;
    for (int j = -r1; j <= r1; j++)
        for (int i = -r1; i <= r1; i++) {
            int d2 = i * i + j * j;
            if (d2 >= r0 * r0 && d2 <= r1 * r1)
                fb_pixel(cx + i, cy + j, color);
        }
}

void fb_blob(int cx, int cy, int R, int a1, int p1, int a2, int p2,
             uint16_t color) {
    /* boundary-walk fill: for each angle step, draw radial line */
    for (int k = 0; k < 256; k++) {
        uint8_t a = (uint8_t)k;
        int rr = R + (R * (a1 * fsin((uint8_t)(3 * a + p1)) +
                           a2 * fsin((uint8_t)(5 * a + p2)))) / (128 * 128);
        int dx = (fcos(a) * rr) / 128, dy = (fsin(a) * rr) / 128;
        /* radial line from center to boundary */
        int steps = dx < 0 ? -dx : dx, sy = dy < 0 ? -dy : dy;
        if (sy > steps) steps = sy;
        if (steps == 0) { fb_pixel(cx, cy, color); continue; }
        for (int s = 0; s <= steps; s++)
            fb_pixel(cx + dx * s / steps, cy + dy * s / steps, color);
    }
}

void fb_waveform(int x0, int x1, int y, int amp, uint8_t phase,
                 uint16_t color) {
    int n = (x1 - x0) / 4;
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * 4;
        uint8_t a = (uint8_t)(phase + i * 9);
        int h = (amp * (128 + fsin(a) + (fsin((uint8_t)(a * 3 + 40)) / 2))) / 256;
        if (h < 2) h = 2;
        fb_fill_rect(x, y - h / 2, 2, h, color);
    }
}

static const uint8_t *glyph_for(char ch) {
    for (int i = 0; i < FONT5X7_COUNT; i++)
        if (font5x7[i].ch == ch) return font5x7[i].col;
    return 0;
}

void fb_text(int x, int y, const char *s, uint16_t color) {
    for (; *s; s++, x += 6) {
        const uint8_t *g = glyph_for(*s);
        if (!g) continue;
        for (int c = 0; c < 5; c++)
            for (int r = 0; r < 7; r++)
                if (g[c] & (1 << r)) fb_pixel(x + c, y + r, color);
    }
}

int fb_text_width(const char *s) {
    int n = 0;
    for (; *s; s++) n++;
    return n ? n * 6 - 1 : 0;
}
