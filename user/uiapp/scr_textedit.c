/*
 * scr_textedit.c - entri teks generik.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"
#include "scr_textedit.h"

/* Semua char harus ada di font5x7 (kapital + digit + simbol dasar). */
static const char *charset =
	" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!.,:-?";

static const char	*te_title = "";
static char		*te_buf = 0;
static unsigned		te_bufsz = 0;
static int		te_masked = 0;
static textedit_done_fn	te_done = 0;
static unsigned		te_pos = 0;	/* 0..len (len = tambah baru) */

static unsigned
slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

static int
ch_idx(char c)
{
	int i = 0;
	while (charset[i]) {
		if (charset[i] == c)
			return i;
		i++;
	}
	return 0;
}

static int
ch_count(void)
{
	int i = 0;
	while (charset[i])
		i++;
	return i;
}

void
textedit_begin(const char *title, char *buf, unsigned bufsz, int masked,
	       textedit_done_fn done)
{
	te_title = title;
	te_buf = buf;
	te_bufsz = bufsz;
	te_masked = masked;
	te_done = done;
	te_pos = slen(buf);
}

static void
te_cycle(int dir)
{
	unsigned len = slen(te_buf);
	int n = ch_count(), i;
	if (te_pos >= len) {
		/* di ujung: tambah huruf baru dulu */
		if (len + 1 >= te_bufsz)
			return;
		te_buf[len] = charset[0];
		te_buf[len + 1] = 0;
		te_pos = len;
	}
	i = ch_idx(te_buf[te_pos]);
	i = (i + dir + n) % n;
	te_buf[te_pos] = charset[i];
}

static void
te_event(int ev)
{
	unsigned len;
	int done_rc = 0, confirmed = 0;
	if (!te_buf)
		return;
	len = slen(te_buf);
	switch (ev) {
	case EV_UP:
		te_cycle(1);
		break;
	case EV_DOWN:
		te_cycle(-1);
		break;
	case EV_LEFT:
		if (te_pos > 0) {
			te_pos--;
		} else if (len > 0) {
			/* di posisi 0: hapus karakter terakhir */
			te_buf[len - 1] = 0;
			te_pos = len - 1;
		}
		break;
	case EV_RIGHT:
		if (te_pos < len) {
			te_pos++;
		} else if (len + 1 < te_bufsz) {
			te_buf[len] = charset[1];	/* 'A' */
			te_buf[len + 1] = 0;
			te_pos = len + 1;
		}
		break;
	case EV_OK:
		done_rc = 1;
		confirmed = 1;
		break;
	case EV_BACK:
		done_rc = 1;
		confirmed = 0;
		break;
	default:
		break;
	}
	if (done_rc) {
		textedit_done_fn d = te_done;
		te_done = 0;
		ui_pop();
		if (d)
			d(confirmed);
	}
}

static void
te_render(void)
{
	unsigned len = te_buf ? slen(te_buf) : 0;
	unsigned i, x;
	int y = 96;

	ui_text_center(30, te_title, C_DIM);

	/* kotak nilai */
	ui_rect(16, y - 14, UI_W - 32, 44, 0x08A5);
	ui_border(16, y - 14, UI_W - 32, 44, C_ACCENT);

	x = 28;
	for (i = 0; i < len && i < 20; i++) {
		if (te_masked) {
			/* mask: kotak kecil, bukan glyph (font tak punya '*') */
			ui_rect((int)x, y + 2, 7, 8,
				i == te_pos ? C_ACCENT : C_FG);
		} else {
			char tmp[2];
			tmp[0] = te_buf[i];
			tmp[1] = 0;
			if (i == te_pos)
				ui_rect((int)x - 1, y - 2, 9, 14, C_SEL);
			ui_text((int)x, y, tmp,
				i == te_pos ? C_ACCENT : C_FG);
		}
		x += 10;
	}
	/* kursor di ujung (tambah baru) */
	if (te_pos >= len && len < 20) {
		if (te_masked)
			ui_rect((int)x, y + 2, 7, 8, C_WARN);
		else
			ui_rect((int)x - 1, y - 2, 9, 14, C_WARN);
	}

	ui_text_center(150, "ATAS/BAWAH: HURUF", C_DIM);
	ui_text_center(164, "KIRI: HAPUS KANAN: TAMBAH", C_DIM);
	ui_text_center(178, "OK: SELESAI BACK: BATAL", C_DIM);
}

const screen_t scr_textedit = {
	0, 0, te_event, te_render
};
