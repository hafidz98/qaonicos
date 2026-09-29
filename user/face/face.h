/* Qabot face model — ported 1:1 in spirit from the "Wajah Agent" web prototype.
 * Procedural renderer: no bitmap assets. Framebuffer is RGB565, 240x240.
 * Portable C (stdint only); the platform provides face_fb + face_flush. */
#ifndef QABOT_FACE_H
#define QABOT_FACE_H

#include <stdint.h>

#define FACE_W 240
#define FACE_H 240

typedef enum { MOOD_DEFAULT, MOOD_TIRED, MOOD_ANGRY, MOOD_HAPPY } face_mood_t;
typedef enum { FACE_EYES, FACE_WAVEFORM, FACE_SPHERE } face_mode_t;

typedef enum {
    EXPR_IDLE,
    EXPR_HAPPY,
    EXPR_ANGRY,
    EXPR_TIRED,
    EXPR_SURPRISED,
    EXPR_LISTENING,
    EXPR_SPEAKING,
    EXPR_THINKING,
    EXPR_COUNT
} face_expr_t;

typedef struct {
    int16_t cx, cy;    /* center */
    int16_t w, h;      /* size */
    int16_t r;         /* corner radius (squircle) */
    int16_t tilt_deg;  /* rotation, 0 = none */
} eye_t;

typedef struct {
    face_mode_t mode;
    face_mood_t mood;
    eye_t eye_l, eye_r;
    uint8_t wave_amp;      /* waveform amplitude 0..64 */
    uint8_t morph_phase;   /* 0..255 animation phase (sphere) */
    uint8_t pulse_phase;   /* 0..255 animation phase (pulse ring) */
    char status_text[24];  /* "LISTENING..." etc. */
    uint8_t show_text;
    /* life */
    uint32_t blink_t_ms;   /* countdown to next blink */
    uint32_t blink_dur_ms;  /* remaining blink time */
    uint32_t gaze_t_ms;    /* countdown to next gaze shift */
    int16_t gaze_x, gaze_y;   /* current gaze offset */
    int16_t gaze_tx, gaze_ty; /* target gaze offset */
    uint16_t rng;          /* xorshift state */
} face_state_t;

void face_init(void);
void face_set_expr(face_expr_t e);
/* Q4: timpa teks status (maks 23 char + NUL); "" = biarkan default expr. */
void face_set_status_text(const char *s);
void face_update(uint32_t dt_ms); /* interpolate + timers, call per frame */
void face_render(void);           /* draw current state into face_fb */
/* Render rows [y0, y0+h) of current state into strip (h*FACE_W RGB565).
 * For partial-framebuffer platforms (App A1). */
void face_render_strip(uint16_t *strip, int y0, int h);

/* Provided by the platform (host SDL test or QaonicOS user program). */
extern uint16_t *face_fb;

#endif /* QABOT_FACE_H */
