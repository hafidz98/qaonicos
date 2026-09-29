/* Headless test: render every expression to a BMP for visual review. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "face.h"
#include "face_draw.h"

static uint16_t fb[FACE_W * FACE_H];

static void write_bmp(const char *path, uint16_t *px, int w, int h) {
    /* 24-bit BMP, bottom-up */
    int rowpad = (4 - (w * 3) % 4) % 4;
    int rowsz = w * 3 + rowpad;
    int imgsz = rowsz * h;
    int fsz = 54 + imgsz;
    unsigned char hdr[54];
    memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = fsz & 0xFF; hdr[3] = (fsz >> 8) & 0xFF;
    hdr[4] = (fsz >> 16) & 0xFF; hdr[5] = (fsz >> 24) & 0xFF;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
    hdr[24] = (h >> 16) & 0xFF; hdr[25] = (h >> 24) & 0xFF;
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = imgsz & 0xFF; hdr[35] = (imgsz >> 8) & 0xFF;
    hdr[36] = (imgsz >> 16) & 0xFF; hdr[37] = (imgsz >> 24) & 0xFF;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(hdr, 1, 54, f);
    unsigned char *row = malloc(rowsz);
    memset(row, 0, rowsz);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t c = px[y * w + x];
            row[x * 3 + 0] = (c & 0x1F) << 3;         /* B */
            row[x * 3 + 1] = ((c >> 5) & 0x3F) << 2;  /* G */
            row[x * 3 + 2] = ((c >> 11) & 0x1F) << 3; /* R */
        }
        fwrite(row, 1, rowsz, f);
    }
    free(row);
    fclose(f);
}

static const char *names[EXPR_COUNT] = {
    "idle", "happy", "angry", "tired",
    "surprised", "listening", "speaking", "thinking"
};

int main(void) {
    face_fb = fb;
    for (int e = 0; e < EXPR_COUNT; e++) {
        face_init();
        face_set_expr((face_expr_t)e);
        /* settle: 60 frames @33ms */
        for (int f = 0; f < 60; f++) face_update(33);
        face_render();
        char path[128];
        snprintf(path, sizeof(path), "/tmp/face_%s.bmp", names[e]);
        write_bmp(path, fb, FACE_W, FACE_H);
        printf("wrote %s\n", path);
    }
    return 0;
}
