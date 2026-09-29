#include "face.h"
#include "face_draw.h"

uint16_t *face_fb = 0;

static face_state_t cur, tgt;

/* palette */
#define COL_BG     rgb565(11, 14, 26)
#define COL_EYE    rgb565(150, 225, 255)
#define COL_EYE_DIM rgb565(90, 140, 170)
#define COL_TEXT   rgb565(200, 210, 225)
#define COL_WAVE   rgb565(110, 200, 255)
#define COL_SPHERE rgb565(120, 190, 255)

static uint16_t rng_next(void) {
    cur.rng ^= (uint16_t)(cur.rng << 7);
    cur.rng ^= (uint16_t)(cur.rng >> 9);
    cur.rng ^= (uint16_t)(cur.rng << 8);
    return cur.rng;
}

static void eye_target(int16_t cx, int16_t cy, int16_t w, int16_t h,
                       int16_t r, int16_t tilt, eye_t *e) {
    e->cx = cx; e->cy = cy; e->w = w; e->h = h; e->r = r; e->tilt_deg = tilt;
}

void face_init(void) {
    cur.rng = 0xACE1;
    for (int i = 0; i < (int)sizeof(cur.status_text); i++) cur.status_text[i] = 0;
    face_set_expr(EXPR_IDLE);
    cur = tgt; /* snap */
    cur.blink_t_ms = 2500;
    cur.gaze_t_ms = 3000;
}

void face_set_expr(face_expr_t e) {
    /* defaults: squircle eyes */
    eye_target(78, 108, 58, 76, 20, 0, &tgt.eye_l);
    eye_target(162, 108, 58, 76, 20, 0, &tgt.eye_r);
    tgt.mode = FACE_EYES;
    tgt.mood = MOOD_DEFAULT;
    tgt.wave_amp = 0;
    tgt.show_text = 0;
    tgt.status_text[0] = 0;

    switch (e) {
    case EXPR_IDLE:
        break;
    case EXPR_HAPPY:
        tgt.mood = MOOD_HAPPY;
        eye_target(78, 102, 64, 42, 20, 0, &tgt.eye_l);
        eye_target(162, 102, 64, 42, 20, 0, &tgt.eye_r);
        break;
    case EXPR_ANGRY:
        tgt.mood = MOOD_ANGRY;
        eye_target(78, 112, 58, 52, 14, -18, &tgt.eye_l);
        eye_target(162, 112, 58, 52, 14, 18, &tgt.eye_r);
        break;
    case EXPR_TIRED:
        tgt.mood = MOOD_TIRED;
        eye_target(78, 116, 58, 34, 16, 0, &tgt.eye_l);
        eye_target(162, 116, 58, 34, 16, 0, &tgt.eye_r);
        break;
    case EXPR_SURPRISED:
        eye_target(78, 108, 72, 94, 26, 0, &tgt.eye_l);
        eye_target(162, 108, 72, 94, 26, 0, &tgt.eye_r);
        break;
    case EXPR_LISTENING:
        tgt.mode = FACE_WAVEFORM;
        tgt.wave_amp = 18;
        tgt.show_text = 1;
        { const char *s = "LISTENING..."; int i = 0;
          while (s[i] && i < 23) { tgt.status_text[i] = s[i]; i++; }
          tgt.status_text[i] = 0; }
        break;
    case EXPR_SPEAKING:
        tgt.mode = FACE_WAVEFORM;
        tgt.wave_amp = 44;
        tgt.show_text = 1;
        { const char *s = "SPEAKING..."; int i = 0;
          while (s[i] && i < 23) { tgt.status_text[i] = s[i]; i++; }
          tgt.status_text[i] = 0; }
        break;
    case EXPR_THINKING:
        tgt.mode = FACE_SPHERE;
        tgt.show_text = 1;
        { const char *s = "THINKING..."; int i = 0;
          while (s[i] && i < 23) { tgt.status_text[i] = s[i]; i++; }
          tgt.status_text[i] = 0; }
        break;
    default:
        break;
    }
}

/* ease current toward target: cur += (tgt-cur)*k */
void
face_set_status_text(const char *s)
{
	int i;
	if (!s || !s[0])
		return;	/* "" = pakai default face_set_expr */
	tgt.show_text = 1;
	for (i = 0; i < 23 && s[i]; i++)
		tgt.status_text[i] = s[i];
	tgt.status_text[i] = 0;
}
static int16_t ease_i16(int16_t c, int16_t t, uint32_t dt_ms) {
    int32_t d = (int32_t)t - c;
    int32_t step = d * (int32_t)dt_ms / 180; /* ~180ms time constant */
    if (step == 0 && d != 0) step = d > 0 ? 1 : -1;
    return (int16_t)(c + step);
}

static void ease_eye(eye_t *c, const eye_t *t, uint32_t dt_ms) {
    c->cx = ease_i16(c->cx, t->cx, dt_ms);
    c->cy = ease_i16(c->cy, t->cy, dt_ms);
    c->w  = ease_i16(c->w,  t->w,  dt_ms);
    c->h  = ease_i16(c->h,  t->h,  dt_ms);
    c->r  = ease_i16(c->r,  t->r,  dt_ms);
    c->tilt_deg = ease_i16(c->tilt_deg, t->tilt_deg, dt_ms);
}

void face_update(uint32_t dt_ms) {
    /* interpolate toward target */
    ease_eye(&cur.eye_l, &tgt.eye_l, dt_ms);
    ease_eye(&cur.eye_r, &tgt.eye_r, dt_ms);
    cur.mode = tgt.mode; /* mode switches are discrete */
    cur.mood = tgt.mood;
    if (cur.wave_amp < tgt.wave_amp) cur.wave_amp++;
    else if (cur.wave_amp > tgt.wave_amp) cur.wave_amp--;
    cur.show_text = tgt.show_text;
    for (int i = 0; i < 24; i++) cur.status_text[i] = tgt.status_text[i];

    /* animation phases */
    cur.morph_phase += (uint8_t)(dt_ms * 256 / 2400);  /* ~2.4s cycle */
    cur.pulse_phase += (uint8_t)(dt_ms * 256 / 1600);  /* ~1.6s cycle */

    /* blink */
    if (cur.blink_dur_ms > 0) {
        cur.blink_dur_ms = cur.blink_dur_ms > dt_ms ? cur.blink_dur_ms - dt_ms : 0;
    } else if (cur.mode == FACE_EYES) {
        if (cur.blink_t_ms > dt_ms) {
            cur.blink_t_ms -= dt_ms;
        } else {
            uint16_t r = rng_next();
            if (cur.mood == MOOD_TIRED) {
                cur.blink_dur_ms = 320;
                cur.blink_t_ms = 4000 + (r % 4000);
            } else {
                cur.blink_dur_ms = 130;
                cur.blink_t_ms = 2400 + (r % 3100);
            }
        }
    }

    /* idle gaze */
    if (cur.gaze_t_ms > dt_ms) {
        cur.gaze_t_ms -= dt_ms;
    } else {
        uint16_t r = rng_next();
        cur.gaze_tx = (int16_t)((r % 29) - 14);
        cur.gaze_ty = (int16_t)(((r >> 5) % 17) - 8);
        cur.gaze_t_ms = 3000 + (rng_next() % 3000);
    }
    cur.gaze_x = ease_i16(cur.gaze_x, cur.gaze_tx, dt_ms);
    cur.gaze_y = ease_i16(cur.gaze_y, cur.gaze_ty, dt_ms);
}

static void draw_eye(const eye_t *e, int16_t gx, int16_t gy, uint16_t color) {
    int16_t cx = e->cx + gx, cy = e->cy + gy;
    int16_t h = e->h;
    /* blink: squash height */
    if (cur.blink_dur_ms > 0 && cur.mode == FACE_EYES) {
        h = 6;
    }
    if (e->tilt_deg != 0)
        fb_round_rect_rot(cx, cy, e->w, h, e->r, e->tilt_deg, color);
    else
        fb_round_rect(cx - e->w / 2, cy - h / 2, e->w, h, e->r, color);
}

void face_render(void) {
    fb_clear_view(COL_BG);

    uint16_t eye_col = (cur.mood == MOOD_TIRED) ? COL_EYE_DIM : COL_EYE;

    if (cur.mode == FACE_EYES) {
        draw_eye(&cur.eye_l, cur.gaze_x, cur.gaze_y, eye_col);
        draw_eye(&cur.eye_r, cur.gaze_x, cur.gaze_y, eye_col);
    } else if (cur.mode == FACE_WAVEFORM) {
        /* talking/listening bars */
        uint8_t ph = cur.pulse_phase;
        int amp = cur.wave_amp;
        /* speaking: envelope wobble */
        if (cur.mood == MOOD_DEFAULT && amp > 30) {
            uint16_t r = cur.rng;
            amp = amp * (70 + (r % 60)) / 100;
        }
        fb_waveform(48, 192, 116, amp, ph, COL_WAVE);
    } else { /* FACE_SPHERE */
        int a1 = 14, a2 = 8;
        fb_blob(120, 108, 42, a1, cur.morph_phase,
                a2, (uint8_t)(255 - cur.morph_phase), COL_SPHERE);
        /* pulse ring */
        int pr = 52 + (10 * ((int)cur.pulse_phase - 128)) / 128;
        uint8_t k = 90 + (uint8_t)((60 * (int)cur.pulse_phase) / 255);
        fb_ring(120, 108, pr < 40 ? 40 : pr, 3, dim565(COL_SPHERE, k));
    }

    /* status text, pulsating */
    if (cur.show_text && cur.status_text[0]) {
        int w = fb_text_width(cur.status_text);
        /* pulse brightness 140..255, triangle wave on pulse_phase */
        uint8_t ph = cur.pulse_phase;
        uint8_t tri = ph < 128 ? (uint8_t)(ph * 2) : (uint8_t)((255 - ph) * 2);
        uint8_t k = (uint8_t)(140 + (115 * (int)tri) / 255);
        fb_text(120 - w / 2, 196, cur.status_text, dim565(COL_TEXT, k));
    }
}

/* Render baris [y0, y0+h) dari state saat ini ke strip buffer
 * (h * FACE_W piksel RGB565). Untuk framebuffer parsial App A1. */
void face_render_strip(uint16_t *strip, int y0, int h) {
    uint16_t *save = face_fb;
    face_fb = strip;
    fb_set_view(y0, h);
    face_render();
    fb_reset_view();
    face_fb = save;
}
