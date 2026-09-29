/*
 * ui.c - screen manager + status bar + render frame uiapp.
 *
 * Status bar permanen (§2.4): jam kiri (uptime HH:MM:SS — data real),
 * baterai kanan (outline saja; tanpa ADC/Fuel gauge = jujur kosong).
 * Digambar di setiap frame di atas semua layar.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

#define STRIP_H		24
#define NSTRIP		(UI_H / STRIP_H)
#define SB_H		16		/* tinggi status bar */

static uint16_t	strip[UI_W * STRIP_H];	/* .bss 11.5 KB */

#define UI_MAX_DEPTH	4
static const screen_t *stk[UI_MAX_DEPTH];
static int		depth = 0;
static int		want_release = 0;

void
ui_push(const screen_t *s)
{
	if (depth >= UI_MAX_DEPTH || s == 0)
		return;
	stk[depth++] = s;
	if (s->enter)
		s->enter();
}

void
ui_pop(void)
{
	if (depth <= 0)
		return;
	depth--;
	if (stk[depth]->exit)
		stk[depth]->exit();
}

void
ui_reset(const screen_t *s)
{
	while (depth > 0)
		ui_pop();
	want_release = 0;
	ui_push(s);
}

int
ui_depth(void)
{
	return depth;
}

const screen_t *
ui_top(void)
{
	return depth > 0 ? stk[depth - 1] : 0;
}

void
ui_request_release(void)
{
	want_release = 1;
}

int
ui_release_requested(void)
{
	return want_release;
}

void
ui_clear_release(void)
{
	want_release = 0;
}

/* HH:MM:SS dari uptime_ms (data real). */
static void
fmt_clock(char *out, unsigned ms)
{
	unsigned s = ms / 1000u;
	unsigned hh = (s / 3600u) % 24u;
	unsigned mm = (s / 60u) % 60u;
	unsigned ss = s % 60u;
	out[0] = (char)('0' + hh / 10u); out[1] = (char)('0' + hh % 10u);
	out[2] = ':';
	out[3] = (char)('0' + mm / 10u); out[4] = (char)('0' + mm % 10u);
	out[5] = ':';
	out[6] = (char)('0' + ss / 10u); out[7] = (char)('0' + ss % 10u);
	out[8] = 0;
}

/* Status bar: y 0..15. */
static void
status_bar(void)
{
	char clk[9];
	ui_rect(0, 0, UI_W, SB_H, 0x08A5);
	ui_rect(0, SB_H - 1, UI_W, 1, C_SEL);
	fmt_clock(clk, sys_uptime());
	ui_text(6, 4, clk, C_FG);
	/* Baterai: outline saja (tanpa ADC = jujur tak ada isi). */
	ui_border(UI_W - 30, 4, 20, 8, C_DIM);
	ui_rect(UI_W - 8, 6, 2, 4, C_DIM);	/* kepala baterai */
}

static void
render_strips(void)
{
	int s;
	const screen_t *t = ui_top();
	for (s = 0; s < NSTRIP; s++) {
		ui_set_strip(strip, s * STRIP_H, STRIP_H);
		ui_clear(C_BG);
		status_bar();
		if (t && t->render)
			t->render();
		if (sys_display_flush(0, (unsigned)(s * STRIP_H), UI_W,
				      STRIP_H, strip, sizeof(strip)) != 0) {
			puts("uiapp FAIL: SYS_DISPLAY_FLUSH\n");
			sys_exit(1);
		}
	}
}

/* Render satu frame penuh (dipanggil tiap FRAME_MS oleh main). */
void
ui_render_frame(void)
{
	render_strips();
}
