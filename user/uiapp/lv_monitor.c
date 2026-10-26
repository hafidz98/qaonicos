/*
 * lv_monitor.c - LVGL Monitor screen (UI-4).
 *
 * Rows (all real data, honest-state rule):
 *   Device  cfg-name (same file the web settings page
 *           and the shell setup command use; "-"
 *           when unreadable)
 *   Uptime  SYS_STAT uptime_ms, seconds
 *   Memory  SYS_STAT mem used/total KB
 *   Threads SYS_STAT nthreads
 *   Net     SYS_STAT rx/tx KB
 *   Disk    SYS_STAT blk used/total sectors, shown
 *           as MB (sector = 512 B, so MB = sec/2048)
 *   CPU     "-" always: the kernel reports
 *           QAON_UNKNOWN (no idle accounting yet)
 *   Boot    bootstate 98 sub 0: boot #, last reason
 *           (0 unknown 1 power 2 crash 3 watchdog
 *           4 software), fail_count
 *   Time    wall clock HH:MM in the configured
 *           timezone (same fmt rule as the status
 *           bar) + NTP sync state (1 synced, else
 *           unsynced); "uptime" clock when no wall
 *           time is set
 * A failed SYS_STAT shows "STAT FAIL" instead of
 * rows; a failed bootstate shows "-" for Boot.
 * Rows refresh on a 500 ms timer; the row texts are
 * printed once on entry (UILV4: mon ...) so verify
 * scripts can assert exact values.
 */
#include "ulib/ulib.h"
#include "lv_port.h"
#include "lv_monitor.h"
#include "lvgl/lvgl.h"

#define TITLE_Y		24
#define ROW_X		10
#define ROW_Y0		46
#define ROW_DY		18
#define BACK_Y		208
#define BACK_W		200
#define BACK_H		24
#define NROWS		9

static lv_group_t *grp;
static int active;
static int exited;
static lv_obj_t *rows[NROWS];
static lv_timer_t *refresh_timer;

static const char *row_keys[NROWS] = {
	"Device", "Uptime", "Memory", "Threads", "Net",
	"Disk", "CPU", "Boot", "Time"
};

/* Decimal print without printf (same pattern as
 * scr_mon.c fmt_u). QAON_UNKNOWN renders as "-". */
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

static void
fmt_clock_hhmm(char *out, unsigned ms)
{
	/* Same rule as the status-bar fmt_clock
	 * (lv_shell.c, ui.c): wall clock in the
	 * configured timezone when set, else uptime. */
	unsigned t = sys_time_get();
	int day, hh, mm;
	if (t != 0u)
		day = (int)(t % 86400u) + 86400 +
		    uilvgl_tz_offset_min() * 60;
	else
		day = (int)((ms / 1000u) % 86400u);
	day %= 86400;
	hh = day / 3600;
	mm = (day / 60) % 60;
	out[0] = (char)('0' + hh / 10); out[1] = (char)('0' + hh % 10);
	out[2] = ':';
	out[3] = (char)('0' + mm / 10); out[4] = (char)('0' + mm % 10);
	out[5] = 0;
}

static const char *
reason_name(unsigned r)
{
	switch (r) {
	case 1: return "power";
	case 2: return "crash";
	case 3: return "watchdog";
	case 4: return "software";
	default: return "unknown";
	}
}

/* Fill vals[0..8] with the current row values. */
static void
collect(char vals[NROWS][48])
{
	struct qaon_stat st;
	unsigned info[4];
	char v0[12], v1[12], clk[6];
	int i;

	for (i = 0; i < NROWS; i++)
		vals[i][0] = 0;
	if (sys_stat(&st) != 0) {
		vals[0][0] = '-'; vals[0][1] = 0;
		return;
	}
	/* Device (the lfs read does not NUL-terminate;
	 * mirror the Settings screen's read_cfg). */
	{
		int n = uilvgl_lfs_read("cfg-name", vals[0],
		    sizeof(vals[0]) - 1u);
		if (n > 0)
			vals[0][n] = 0;
		else {
			vals[0][0] = '-'; vals[0][1] = 0;
		}
	}
	/* Uptime (seconds) */
	fmt_u(v0, st.uptime_ms / 1000u);
	i = 0;
	while (v0[i] && i < 40) { vals[1][i] = v0[i]; i++; }
	vals[1][i++] = ' '; vals[1][i++] = 's'; vals[1][i] = 0;
	/* Memory used/total KB */
	fmt_u(v0, st.mem_used_kb);
	fmt_u(v1, st.mem_total_kb);
	i = 0;
	while (v0[i] && i < 18) { vals[2][i] = v0[i]; i++; }
	vals[2][i++] = '/';
	{
		int j = 0;
		while (v1[j] && i < 40) { vals[2][i++] = v1[j++]; }
	}
	vals[2][i++] = ' '; vals[2][i++] = 'K'; vals[2][i++] = 'B';
	vals[2][i] = 0;
	/* Threads */
	fmt_u(vals[3], st.nthreads);
	/* Net rx/tx KB */
	fmt_u(v0, st.net_rx_kb);
	fmt_u(v1, st.net_tx_kb);
	i = 0;
	while (v0[i] && i < 18) { vals[4][i] = v0[i]; i++; }
	vals[4][i++] = '/';
	{
		int j = 0;
		while (v1[j] && i < 40) { vals[4][i++] = v1[j++]; }
	}
	vals[4][i++] = ' '; vals[4][i++] = 'K'; vals[4][i++] = 'B';
	vals[4][i] = 0;
	/* Disk used/total MB (512 B sectors) */
	if (st.blk_used_sec == QAON_UNKNOWN ||
	    st.blk_total_sec == QAON_UNKNOWN) {
		vals[5][0] = '-'; vals[5][1] = 0;
	} else {
		fmt_u(v0, st.blk_used_sec / 2048u);
		fmt_u(v1, st.blk_total_sec / 2048u);
		i = 0;
		while (v0[i] && i < 18) { vals[5][i] = v0[i]; i++; }
		vals[5][i++] = '/';
		{
			int j = 0;
			while (v1[j] && i < 40) { vals[5][i++] = v1[j++]; }
		}
		vals[5][i++] = ' '; vals[5][i++] = 'M'; vals[5][i++] = 'B';
		vals[5][i] = 0;
	}
	/* CPU: the kernel has no idle accounting yet. */
	vals[6][0] = '-'; vals[6][1] = 0;
	/* Boot: #, reason, fail_count */
	if (sys_bootinfo(info) != 0) {
		vals[7][0] = '-'; vals[7][1] = 0;
	} else {
		const char *rn = reason_name(info[3]);
		fmt_u(v0, info[0]);
		i = 0;
		vals[7][i++] = '#';
		while (v0[i - 1] && i < 20) { vals[7][i] = v0[i - 1]; i++; }
		vals[7][i++] = ' ';
		while (*rn && i < 34) { vals[7][i++] = *rn++; }
		vals[7][i++] = ' ';
		vals[7][i++] = 'F';
		fmt_u(v0, info[1]);
		{
			int j = 0;
			while (v0[j] && i < 44) { vals[7][i++] = v0[j++]; }
		}
		vals[7][i] = 0;
	}
	/* Time: clock + sync state */
	fmt_clock_hhmm(clk, sys_uptime());
	i = 0;
	while (clk[i] && i < 10) { vals[8][i] = clk[i]; i++; }
	vals[8][i++] = ' ';
	{
		const char *s = sys_time_quality() == 1 ?
		    "synced" : "unsynced";
		while (*s && i < 44) { vals[8][i++] = *s++; }
	}
	vals[8][i] = 0;
}

static void
refresh(int doprint)
{
	char vals[NROWS][48];
	char line[64];
	int i, j, k;

	collect(vals);
	for (i = 0; i < NROWS; i++) {
		k = 0;
		while (row_keys[i][k] && k < 12) { line[k] = row_keys[i][k]; k++; }
		line[k++] = ':'; line[k++] = ' ';
		j = 0;
		while (vals[i][j] && k < 60) { line[k++] = vals[i][j++]; }
		line[k] = 0;
		lv_label_set_text(rows[i], line);
		if (doprint) {
			puts("UILV4: mon ");
			puts(line);
			puts("\n");
		}
	}
}

static void
refresh_timer_cb(lv_timer_t *t)
{
	(void)t;
	refresh(0);
}

static void
back_btn_cb(lv_event_t *e)
{
	(void)e;
	exited = 1;
	puts("UILV4: monitor back\n");
}

void
lvmon_enter(lv_group_t *g)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *title, *btn, *lbl;
	int i;

	grp = g;
	active = 1;
	exited = 0;
	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	title = lv_label_create(scr);
	lv_label_set_text(title, "Monitor");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
	for (i = 0; i < NROWS; i++) {
		rows[i] = lv_label_create(scr);
		lv_label_set_text(rows[i], row_keys[i]);
		lv_obj_align(rows[i], LV_ALIGN_TOP_LEFT, ROW_X,
			     ROW_Y0 + i * ROW_DY);
	}
	btn = lv_button_create(scr);
	lv_obj_set_size(btn, BACK_W, BACK_H);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, BACK_Y);
	lv_obj_set_style_pad_top(btn, 2, 0);
	lv_obj_set_style_pad_bottom(btn, 2, 0);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "BACK");
	lv_obj_center(lbl);
	lv_group_add_obj(grp, btn);
	lv_obj_add_event_cb(btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_group_focus_obj(btn);
	refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
	refresh(1);
	puts("UILV4: monitor\n");
}

int
lvmon_tick(void)
{
	int e = exited;
	if (e)
		exited = 0;
	return e;
}

int
lvmon_on_back(void)
{
	/* Root view: BACK exits to the shell menu. */
	return 0;
}

int
lvmon_active(void)
{
	return active;
}

void
lvmon_exit(void)
{
	if (refresh_timer) {
		lv_timer_delete(refresh_timer);
		refresh_timer = NULL;
	}
	active = 0;
	exited = 0;
}
