/*
 * scr_mon.c - system monitor: data REAL via SYS_STAT (57).
 *
 * Uptime, thread, memori, storage, net — langsung dari kernel.
 * QAON_UNKNOWN (0xFFFFFFFF) ditampilkan sebagai "-" (jujur).
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"

#define Y0	44
#define LH	18

static void
fmt_u(char *out, unsigned v)
{
	char tmp[12];
	int n = 0, i;
	if (v == QAON_UNKNOWN) {
		out[0] = '-'; out[1] = 0;
		return;
	}
	if (v == 0) {
		out[0] = '0'; out[1] = 0;
		return;
	}
	while (v > 0 && n < 11) {
		tmp[n++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	out[n] = 0;
}

/* Baris "KEY VALUE" di x=20. */
static void
row(int y, const char *key, const char *val)
{
	char line[36];
	int i = 0;
	while (*key && i < 20)
		line[i++] = *key++;
	while (i < 14)
		line[i++] = ' ';
	while (*val && i < 34)
		line[i++] = *val++;
	line[i] = 0;
	ui_text(20, y, line, C_FG);
}

static void
mon_event(int ev)
{
	if (ev == EV_BACK)
		ui_pop();
}

static void
mon_render(void)
{
	struct qaon_stat st;
	char v0[12], v1[12], both[26];
	int y = Y0, i;

	ui_text_center(22, "MONITOR", C_DIM);
	if (sys_stat(&st) != 0) {
		ui_text_center(90, "STAT GAGAL", C_WARN);
		return;
	}

	fmt_u(v0, st.uptime_ms / 1000u);
	row(y, "UPTIME S", v0); y += LH;

	fmt_u(v0, st.nthreads);
	row(y, "THREAD", v0); y += LH;

	fmt_u(v0, st.mem_used_kb);
	fmt_u(v1, st.mem_total_kb);
	i = 0;
	while (v0[i]) { both[i] = v0[i]; i++; }
	both[i++] = '/';
	{
		int j = 0;
		while (v1[j]) both[i++] = v1[j++];
		both[i] = 0;
	}
	row(y, "MEM KB", both); y += LH;

	fmt_u(v0, st.blk_total_sec);
	row(y, "BLK SEC", v0); y += LH;

	fmt_u(v0, st.net_rx_kb);
	fmt_u(v1, st.net_tx_kb);
	i = 0;
	while (v0[i]) { both[i] = v0[i]; i++; }
	both[i++] = '/';
	{
		int j = 0;
		while (v1[j]) both[i++] = v1[j++];
		both[i] = 0;
	}
	row(y, "NET KB", both); y += LH;

	ui_text(20, y + 6, "CPU -", C_DIM);	/* belum ada idle accounting */
}

const screen_t scr_mon = {
	0, 0, mon_event, mon_render
};
