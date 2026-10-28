/*
 * lv_shell.c - LVGL shell: status bar + menu (UI-2).
 *
 * One screen object, rebuilt on every navigation step
 * (menu <-> submenu/screen), so no screen is ever leaked
 * across switches. The status bar lives on the top
 * layer, above all screens, and is deleted with the
 * display at teardown. The focus group is the port's
 * shared group: every rebuild removes all objects and
 * re-adds the new screen's focusables.
 *
 * Honest-state rule (v4 restyle): the clock is live
 * (wall clock WIB when set, else uptime); the radio
 * icons follow linkd's live WIFI/BLE state via the
 * K5 poll timer (QEMU without a peer still shows
 * OFF honestly -- a failed query is OFF, never a
 * stale ON); the battery is an outline only (no ADC
 * level exists). A failed flush is counted, never
 * fatal (same rule as the UI-1 smoke screen).
 *
 * Navigation contract (UI-2, extended UI-3/UI-4):
 * the LVGL menu replaces the old hand-rolled menu
 * and settings-list screens. Leaf screens (Wi-Fi,
 * Bluetooth, LLM, Passkey) still run hand-rolled
 * under the same token grant and return here when
 * popped. Settings is the UI-3 LVGL submenu
 * (lv_settings.c); Monitor (lv_monitor.c) and Power
 * (lv_power.c) are UI-4 LVGL screens. No placeholder
 * entries remain in UI-4.
 */
#include "ulib/ulib.h"
#include "ulib/uipc.h"
#include "lv_port.h"
#include "lv_shell.h"
#include "lv_settings.h"
#include "lv_monitor.h"
#include "lv_power.h"
#include "lvgl/lvgl.h"
#include "uartproto/uartproto.h"
#include "uartproto/linkd.h"

#define UI_W		240
#define BAR_H		20
#define MENU_Y0		24
#define BTN_W		200
#define BTN_H		24
#define BTN_GAP		2
#define IDLE_LIMIT_MS	30000u		/* mirrors UI_IDLE_RETURN_S */

/* Menu entry kinds. SETTINGS is the UI-3 submenu,
 * owned by lv_settings.c; MONITOR and POWER are
 * the UI-4 screens, owned by lv_monitor.c and
 * lv_power.c. */
#define KIND_RELEASE	0
#define KIND_OLD	1
#define KIND_SETTINGS	2
#define KIND_MONITOR	3
#define KIND_POWER	4

struct shell_entry {
	const char *label;
	const char *key;
	int kind;
	int action;		/* SHELL_* for KIND_OLD */
};

static const struct shell_entry entries[] = {
	{ "Qabot", "qabot", KIND_RELEASE, SHELL_RELEASE },
	{ "Wi-Fi", "wifi", KIND_OLD, SHELL_WIFI },
	{ "Bluetooth", "ble", KIND_OLD, SHELL_BLE },
	{ "LLM", "llm", KIND_OLD, SHELL_LLM },
	{ "Passkey", "passkey", KIND_OLD, SHELL_PASSKEY },
	{ "Settings", "settings", KIND_SETTINGS, 0 },
	{ "Monitor", "monitor", KIND_MONITOR, 0 },
	{ "Power", "power", KIND_POWER, 0 },
};
#define NENTRIES	(sizeof(entries) / sizeof(entries[0]))

#define VIEW_MENU	0
#define VIEW_SETTINGS	1
#define VIEW_MONITOR	2
#define VIEW_POWER	3

static lv_group_t *grp;
static lv_obj_t *time_label;
static lv_timer_t *clock_timer;

/* Live radio-state poll (K5). The status-bar icons
 * are LVGL labels owned by the port; this file owns
 * the cache they render. Design (see DEVLOG-K5):
 * - The poll runs on an LVGL timer in the shell loop,
 *   NEVER in the menu-build path: build_status_bar()
 *   only creates the timer (initial icons OFF, the
 *   honest QEMU baseline), so menu appearance costs
 *   exactly zero RPCs even with linkd dead.
 * - First fire is one full period after build, so the
 *   menu renders long before any poll can stall the
 *   loop. Period 5 s: radio state changes on human
 *   timescales, and a shorter period would only buy
 *   UI stalls.
 * - Each poll is a STATUS gate (no wire) + at most
 *   two short-timeout STATUS commands. A failed or
 *   unknown query forces that icon OFF, never a stale
 *   ON: an icon asserts live state, and OFF is the
 *   safe default (it is also the QEMU baseline).
 * - The timer is deleted at shell end next to the
 *   clock timer, so no poll can run without a
 *   display. The UI-1 smoke screen never builds a
 *   status bar, so it stays poll-free. */
#define RADIO_PERIOD_MS		5000u
#define RADIO_RPC_MS		3000u
static lv_timer_t *radio_timer;
static int radio_wifi_on;
static int radio_bt_on;
static int view;
static int settings_requested;
static int settings_active;
static int monitor_requested;
static int monitor_active;
static int power_requested;
static int power_active;
static int pending_action;
static int shell_done;

static void
fmt_clock(char *out, unsigned ms)
{
	/* Same rule as ui.c fmt_clock: wall clock in the
	 * configured timezone (cfg-tz via
	 * uilvgl_tz_offset_min(), WIB fallback, UI-4)
	 * when set, else uptime. No seconds (owner
	 * feedback 2026-10-22). The +86400 keeps the
	 * day sum positive for negative offsets without
	 * any 32-bit overflow (day seconds < 86400). */
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
	out[0] = (char)('0' + hh / 10u); out[1] = (char)('0' + hh % 10u);
	out[2] = ':';
	out[3] = (char)('0' + mm / 10u); out[4] = (char)('0' + mm % 10u);
	out[5] = 0;
}

/* STATUS gate: 1 = transport UP, 0 = DOWN/absent.
 * Mirrors the shell `link` command's construction
 * (uipc_msg header is 6 words; body word 0 = op,
 * word 1 = state). Polarity is UPROTO_LINK_UP = 0 /
 * DOWN = 1 per the normative uartproto.h -- the
 * LINKD-CONTRACT prose line "0 DOWN / 1 UP" says
 * the opposite and is wrong (see DEVLOG-K5); the
 * shell, the header, and the C6L-5 "link: UP"
 * expectation all agree on UP = 0. */
static int
radio_link_up(void)
{
	static unsigned words[128];
	struct uipc_msg *m = (struct uipc_msg *)words;
	unsigned i;
	int rc;

	for (i = 0u; i < 128u; i++)
		words[i] = 0u;
	m->id = (int)LINKD_OP_STATUS;
	m->size = LINKD_BUDGET_STATUS;
	words[6u] = LINKD_OP_STATUS;
	rc = uipc_rpc_user("link", m, LINKD_BUDGET_STATUS,
			   (int)RADIO_RPC_MS);
	if (rc < 0 || words[6u] != LINKD_OK)
		return 0;
	return words[7u] == (unsigned)UPROTO_LINK_UP;
}

/* "CONNECTED <ssid>" -> ON, anything else -> OFF. */
static int
radio_parse_wifi(const char *resp)
{
	static const char *want = "CONNECTED";
	unsigned i;

	for (i = 0u; i < 9u; i++) {
		if (resp[i] != want[i])
			return 0;
	}
	return 1;
}

/* "ON ..." -> ON, "OFF ..." -> OFF, anything else
 * -> OFF. The first two bytes discriminate: "ON"
 * vs "OF". */
static int
radio_parse_bt(const char *resp)
{
	return resp[0] == 'O' && resp[1] == 'N';
}

static void
radio_timer_cb(lv_timer_t *t)
{
	char resp[64];
	int wifi, bt, changed;

	(void)t;
	if (!radio_link_up()) {
		/* Linkd absent or transport DOWN: no
		 * wire RPCs at all (a DOWN transport
		 * would only buy stalls), both icons
		 * OFF. This is the QEMU default and
		 * the failure case alike. */
		wifi = 0;
		bt = 0;
	} else {
		/* Per-icon independence: a wifi-only
		 * failure must not kill the bt icon
		 * (and vice versa); each failure
		 * forces only its own icon OFF. */
		if (uproto_cmd_t("WIFI.STATUS", resp, sizeof(resp),
				 RADIO_RPC_MS) == 0)
			wifi = radio_parse_wifi(resp);
		else
			wifi = 0;
		if (uproto_cmd_t("BLE.STATUS", resp, sizeof(resp),
				 RADIO_RPC_MS) == 0)
			bt = radio_parse_bt(resp);
		else
			bt = 0;
	}
	changed = (wifi != radio_wifi_on || bt != radio_bt_on);
	radio_wifi_on = wifi;
	radio_bt_on = bt;
	uilvgl_set_radio_icons(wifi, bt);
	/* Change-only logging: a line per poll would
	 * spam the serial console and pollute other
	 * scripts' markers. */
	if (changed) {
		/* NOTE: not UILV5: -- verify-ui5.py asserts
		 * no UILV5: marker survives (the removed
		 * name editor's namespace). RADIO: is
		 * unambiguous. */
		puts("RADIO: wifi=");
		puts(wifi ? "1 bt=" : "0 bt=");
		puts(bt ? "1\n" : "0\n");
	}
}

static void
clock_timer_cb(lv_timer_t *t)
{
	char clk[6];

	/* lv_timer_t is opaque in the public headers, so
	 * the label is reached via the file static, not
	 * through user_data. */
	(void)t;
	if (!time_label)
		return;
	fmt_clock(clk, sys_uptime());
	lv_label_set_text(time_label, clk);
}

static void
focus_cb(lv_event_t *e)
{
	puts("UILV2: focus ");
	puts((const char *)lv_event_get_user_data(e));
	puts("\n");
}

static void
activate_cb(lv_event_t *e)
{
	const struct shell_entry *en =
	    (const struct shell_entry *)lv_event_get_user_data(e);
	puts("UILV2: activate ");
	puts(en->key);
	puts("\n");
	if (en->kind == KIND_RELEASE) {
		pending_action = SHELL_RELEASE;
		shell_done = 1;
	} else if (en->kind == KIND_OLD) {
		pending_action = en->action;
		shell_done = 1;
	} else if (en->kind == KIND_SETTINGS) {
		/* Submenu/screen: handled by switching
		 * screens in the loop below (the callback
		 * only marks it; the loop owns rebuilds). */
		settings_requested = 1;
	} else if (en->kind == KIND_MONITOR) {
		monitor_requested = 1;
	} else {
		power_requested = 1;
	}
}

static lv_obj_t *
mk_menu_button(lv_obj_t *scr, int idx, const struct shell_entry *en)
{
	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_t *lbl;
	lv_obj_set_size(btn, BTN_W, BTN_H);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0,
		     MENU_Y0 + idx * (BTN_H + BTN_GAP));
	/* 14 px font in a 24 px button: shrink the theme
	 * padding so the label is not clipped. */
	lv_obj_set_style_pad_top(btn, 2, 0);
	lv_obj_set_style_pad_bottom(btn, 2, 0);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, en->label);
	lv_obj_center(lbl);
	lv_group_add_obj(grp, btn);
	lv_obj_add_event_cb(btn, focus_cb, LV_EVENT_FOCUSED,
			    (void *)en->key);
	lv_obj_add_event_cb(btn, activate_cb, LV_EVENT_CLICKED,
			    (void *)en);
	return btn;
}

/* Rebuild the single screen object as the menu. The
 * group is emptied first: deleting grouped objects
 * without removing them is not something to rely on. */
static void
build_menu(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *first = NULL, *btn;
	unsigned i;

	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	for (i = 0; i < NENTRIES; i++) {
		btn = mk_menu_button(scr, (int)i, &entries[i]);
		if (i == 0)
			first = btn;
	}
	if (first)
		lv_group_focus_obj(first);
	view = VIEW_MENU;
}

static void
build_status_bar(void)
{
	lv_obj_t *top = lv_layer_top();
	lv_obj_t *bg;
	char clk[6];

	/* Bar background: flat light gray, square
	 * corners, no border — a separator, not a tile. */
	bg = lv_obj_create(top);
	lv_obj_set_size(bg, UI_W, BAR_H);
	lv_obj_align(bg, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_radius(bg, 0, 0);
	lv_obj_set_style_border_width(bg, 0, 0);
	lv_obj_set_style_pad_all(bg, 0, 0);
	lv_obj_set_style_bg_color(bg, lv_color_hex(0xd8d8d8), 0);

	time_label = lv_label_create(top);
	fmt_clock(clk, sys_uptime());
	lv_label_set_text(time_label, clk);
	lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 6, 2);

	/* Radio + battery icons (UI-8) are icon-font
	 * labels created by the port (Material Symbols
	 * subset; see lv_port.c). Initial state: wifi +
	 * bluetooth OFF (the font's real off glyphs).
	 * Live state arrives via the K5 radio poll
	 * timer below -- never a blocking RPC in the
	 * menu-build path, so menu appearance costs
	 * zero RPCs even with linkd dead. Battery is
	 * outline-only (no ADC, never a fake level). */
	radio_wifi_on = 0;
	radio_bt_on = 0;
	uilvgl_set_radio_icons(0, 0);
	radio_timer = lv_timer_create(radio_timer_cb,
				       RADIO_PERIOD_MS, NULL);

	clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
}

int
uilvgl_shell_run(void)
{
	unsigned last, now;

	grp = uilvgl_shell_begin();
	build_status_bar();
	build_menu();

	pending_action = -1;
	settings_requested = 0;
	settings_active = 0;
	monitor_requested = 0;
	monitor_active = 0;
	power_requested = 0;
	power_active = 0;
	shell_done = 0;
	last = sys_uptime();
	puts("UILV2: start\n");

	while (sys_display_status() == 1 && !shell_done) {
		now = sys_uptime();
		lv_tick_inc(now - last);
		last = now;
		lv_timer_handler();
		/* Submenu/screen activation is a screen
		 * switch, done here (not in the click
		 * callback) so the group/screen rebuild
		 * never runs inside an event handler for
		 * a deleted object. */
		if (settings_requested && view == VIEW_MENU) {
			settings_requested = 0;
			settings_active = 1;
			lvset_enter(grp);
			view = VIEW_SETTINGS;
		} else if (settings_active) {
			if (lvset_tick()) {
				/* List BACK button: back to menu.
				 * Same marker as the BACK-key
				 * path below (UI-5 polish: the
				 * two BACK paths were
				 * observably different). */
				settings_active = 0;
				view = VIEW_MENU;
				build_menu();
				puts("UILV3: back menu\n");
			}
		}
		if (monitor_requested && view == VIEW_MENU) {
			monitor_requested = 0;
			monitor_active = 1;
			lvmon_enter(grp);
			view = VIEW_MONITOR;
		} else if (monitor_active) {
			if (lvmon_tick()) {
				/* BACK button: back to menu. The
				 * exit deletes the refresh timer:
				 * without it the timer would keep
				 * writing to labels build_menu
				 * just deleted. Same back-menu
				 * marker as the BACK-key path
				 * (UI-5 polish). */
				monitor_active = 0;
				lvmon_exit();
				view = VIEW_MENU;
				build_menu();
				puts("UILV4: back menu\n");
			}
		}
		if (power_requested && view == VIEW_MENU) {
			power_requested = 0;
			power_active = 1;
			lvpower_enter(grp);
			view = VIEW_POWER;
		} else if (power_active) {
			int pr = lvpower_tick();
			if (pr == 1) {
				/* BACK button: back to menu.
				 * Same back-menu marker as the
				 * BACK-key path (UI-5 polish). */
				power_active = 0;
				lvpower_exit();
				view = VIEW_MENU;
				build_menu();
				puts("UILV4: back menu\n");
			} else if (pr == 2) {
				/* Sleep Now: display already
				 * blanked; release the token. */
				power_active = 0;
				pending_action = SHELL_RELEASE;
				shell_done = 1;
				puts("UILV2: release\n");
			}
		}
		if (uilvgl_take_back()) {
			if (settings_active) {
				if (!lvset_on_back()) {
					settings_active = 0;
					view = VIEW_MENU;
					build_menu();
					puts("UILV3: back menu\n");
				}
			} else if (monitor_active) {
				if (!lvmon_on_back()) {
					monitor_active = 0;
					lvmon_exit();
					view = VIEW_MENU;
					build_menu();
					puts("UILV4: back menu\n");
				}
			} else if (power_active) {
				if (!lvpower_on_back()) {
					power_active = 0;
					lvpower_exit();
					view = VIEW_MENU;
					build_menu();
					puts("UILV4: back menu\n");
				}
			} else {
				pending_action = SHELL_RELEASE;
				shell_done = 1;
				puts("UILV2: release\n");
			}
		}
		if (now - uilvgl_last_input_ms() >= IDLE_LIMIT_MS) {
			puts("UILV2: idle exit\n");
			pending_action = SHELL_RELEASE;
			shell_done = 1;
		}
		sys_yield();
	}

	puts("UILV2: exit\n");
	if (uilvgl_flush_failures() != 0) {
		/* Decimal print without printf (same pattern
		 * as the smoke screen's failure report). */
		char nbuf[12];
		unsigned v = uilvgl_flush_failures(), i = 0, j;
		puts("UILV2: flush failures: ");
		if (v == 0)
			nbuf[i++] = '0';
		while (v > 0) {
			nbuf[i++] = (char)('0' + v % 10u);
			v /= 10u;
		}
		for (j = 0; j < i / 2u; j++) {
			char t = nbuf[j];
			nbuf[j] = nbuf[i - 1u - j];
			nbuf[i - 1u - j] = t;
		}
		nbuf[i] = 0;
		puts(nbuf);
		puts("\n");
	}
	if (radio_timer) {
		lv_timer_delete(radio_timer);
		radio_timer = NULL;
	}
	if (clock_timer) {
		lv_timer_delete(clock_timer);
		clock_timer = NULL;
	}
	lvset_exit();
	lvmon_exit();
	lvpower_exit();
	uilvgl_shell_end();
	return pending_action < 0 ? SHELL_RELEASE : pending_action;
}
