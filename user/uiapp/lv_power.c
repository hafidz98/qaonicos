/*
 * lv_power.c - LVGL Power screen (UI-4).
 *
 * Same actions, same order, same semantics as the
 * hand-rolled scr_power screen (see lv_power.h):
 * SHUTDOWN / RESTART / SLEEP NOW / BACK. Focus
 * starts on SHUTDOWN, matching the old screen's
 * sel = 0. Button callbacks only mark state; the
 * tick runs the action so no syscall runs inside
 * an event handler for a deleted object.
 */
#include "ulib/ulib.h"
#include "lv_port.h"
#include "lv_power.h"
#include "lvgl/lvgl.h"

#define TITLE_Y		24
#define BTN_Y0		48
#define BTN_H		24
#define BTN_GAP		2
#define BTN_W		200
#define STATUS_Y	168

#define ACT_NONE	0
#define ACT_SHUTDOWN	1
#define ACT_RESTART	2
#define ACT_SLEEP	3
#define ACT_BACK	4

static lv_group_t *grp;
static int active;
static int pending;	/* ACT_* requested by a callback */
static lv_obj_t *btns[4];
static lv_obj_t *status_label;

static const char *btn_labels[4] = {
	"SHUTDOWN", "RESTART", "SLEEP NOW", "BACK"
};

/* Focus-marker keys (shell-menu style, lowercase). */
static const char *btn_keys[4] = {
	"shutdown", "restart", "sleep", "back"
};

static void
focus_cb(lv_event_t *e)
{
	/* Mirror the shell menu's focus markers so
	 * navigation is observable on the console. */
	puts("UILV4: focus ");
	puts((const char *)lv_event_get_user_data(e));
	puts("\n");
}

static void
activate_cb(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_target(e);
	int i;

	for (i = 0; i < 4; i++) {
		if (t == btns[i]) {
			pending = ACT_SHUTDOWN + i;
			break;
		}
	}
}

void
lvpower_enter(lv_group_t *g)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *title, *btn, *lbl;
	int i;

	grp = g;
	active = 1;
	pending = ACT_NONE;
	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	title = lv_label_create(scr);
	lv_label_set_text(title, "Power");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
	for (i = 0; i < 4; i++) {
		btn = lv_button_create(scr);
		lv_obj_set_size(btn, BTN_W, BTN_H);
		lv_obj_align(btn, LV_ALIGN_TOP_MID, 0,
			     BTN_Y0 + i * (BTN_H + BTN_GAP));
		lv_obj_set_style_pad_top(btn, 2, 0);
		lv_obj_set_style_pad_bottom(btn, 2, 0);
		lbl = lv_label_create(btn);
		lv_label_set_text(lbl, btn_labels[i]);
		lv_obj_center(lbl);
		lv_group_add_obj(grp, btn);
		lv_obj_add_event_cb(btn, focus_cb, LV_EVENT_FOCUSED,
				    (void *)btn_keys[i]);
		lv_obj_add_event_cb(btn, activate_cb,
				    LV_EVENT_CLICKED, NULL);
		btns[i] = btn;
	}
	status_label = lv_label_create(scr);
	lv_label_set_text(status_label, "");
	lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, STATUS_Y);
	lv_group_focus_obj(btns[0]);
	puts("UILV4: power\n");
}

int
lvpower_tick(void)
{
	int act = pending;

	if (act == ACT_NONE)
		return 0;
	pending = ACT_NONE;
	switch (act) {
	case ACT_SHUTDOWN:
		puts("UILV4: power shutdown\n");
		puts("uiapp: shutdown (SYS_POWEROFF)\n");
		(void)sys_poweroff();
		/* Platform refused: stay usable. */
		lv_label_set_text(status_label, "Shutdown refused");
		puts("UILV4: power shutdown refused\n");
		return 0;
	case ACT_RESTART:
		puts("UILV4: power restart\n");
		puts("uiapp: restart (SYS_RESET)\n");
		(void)sys_reset();
		/* Platform refused: stay usable. */
		lv_label_set_text(status_label, "Restart refused");
		puts("UILV4: power restart refused\n");
		return 0;
	case ACT_SLEEP:
		puts("UILV4: power sleep\n");
		(void)sys_display_sleep(1);
		return 2;
	default:	/* ACT_BACK */
		puts("UILV4: power back\n");
		return 1;
	}
}

int
lvpower_on_back(void)
{
	/* Root view: BACK exits to the shell menu. */
	return 0;
}

int
lvpower_active(void)
{
	return active;
}

void
lvpower_exit(void)
{
	active = 0;
	pending = ACT_NONE;
}
