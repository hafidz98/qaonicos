/*
 * ui.c - screen manager + status bar + render frame uiapp.
 *
 * Status bar permanen (§2.4): jam kiri HH:MM (jam dinding WIB
 * via NTP bila tersedia, fallback uptime), kanan ikon Wi-Fi +
 * Bluetooth (redup = belum ada link) dan baterai (outline saja;
 * tanpa ADC/Fuel gauge = jujur kosong).
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

/* HH:MM (owner feedback 2026-10-22: no seconds). Wall clock
 * (NTP, WIB = UTC+7) when available, else uptime. */
static void
fmt_clock(char *out, unsigned ms)
{
	unsigned t = sys_time_get();
	unsigned s, hh, mm;
	if (t != 0u)
		s = t + 7u * 3600u;	/* WIB */
	else
		s = ms / 1000u;
	hh = (s / 3600u) % 24u;
	mm = (s / 60u) % 60u;
	out[0] = (char)('0' + hh / 10u); out[1] = (char)('0' + hh % 10u);
	out[2] = ':';
	out[3] = (char)('0' + mm / 10u); out[4] = (char)('0' + mm % 10u);
	out[5] = 0;
}

/* Wi-Fi fan glyph (~15x11) at (x, y): three arcs + dot,
 * hand-tabled pixel offsets from the dot centre. */
static void
icon_wifi(int x, int y, uint16_t c)
{
	static const signed char a1[] = {0,-3, -2,-2, 2,-2, -3,-1, 3,-1};
	static const signed char a2[] = {0,-5, -4,-4, 4,-4, -5,-2, 5,-2};
	static const signed char a3[] = {0,-7, -5,-5, 5,-5, -7,-2, 7,-2};
	int i;
	for (i = 0; i < 10; i++) {
		ui_px(x + 7 + a1[i * 2], y + 9 + a1[i * 2 + 1], c);
		ui_px(x + 7 + a2[i * 2], y + 9 + a2[i * 2 + 1], c);
	}
	for (i = 0; i < 8; i++)
		ui_px(x + 7 + a3[i * 2], y + 9 + a3[i * 2 + 1], c);
	ui_rect(x + 6, y + 8, 2, 2, c);
}

/* Bluetooth rune glyph (~8x11) at (x, y). */
static void
icon_bt(int x, int y, uint16_t c)
{
	int i;
	for (i = 0; i <= 10; i++)
		ui_px(x + 3, y + i, c);		/* spine */
	/* upper triangle edges */
	ui_px(x + 4, y + 0, c); ui_px(x + 5, y + 1, c);
	ui_px(x + 6, y + 1, c); ui_px(x + 7, y + 2, c);
	ui_px(x + 6, y + 3, c); ui_px(x + 5, y + 3, c);
	ui_px(x + 4, y + 4, c);
	/* lower triangle edges */
	ui_px(x + 4, y + 6, c); ui_px(x + 5, y + 6, c);
	ui_px(x + 6, y + 7, c); ui_px(x + 7, y + 8, c);
	ui_px(x + 6, y + 9, c); ui_px(x + 5, y + 9, c);
	ui_px(x + 4, y + 10, c);
}

/* Status bar: y 0..15. Right side: Wi-Fi + Bluetooth +
 * battery. The radio icons are drawn DIM: no radio link
 * state exists under QEMU (the C6 will drive them on
 * hardware) -- same honesty rule as the empty battery. */
static void
status_bar(void)
{
	char clk[6];
	ui_rect(0, 0, UI_W, SB_H, 0x08A5);
	ui_rect(0, SB_H - 1, UI_W, 1, C_SEL);
	fmt_clock(clk, sys_uptime());
	ui_text(6, 4, clk, C_FG);
	icon_wifi(UI_W - 66, 3, C_DIM);
	icon_bt(UI_W - 46, 3, C_DIM);
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
