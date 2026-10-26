/*
 * lv_settings.c - LVGL Settings screen (UI-3, name
 * editor UI-5).
 *
 * Views, all rebuilt on the single screen object
 * like the shell menu (never inside an event
 * handler): list -> detail (name/tz/pin) -> picker
 * (PIN length + digit entry) -> editor (UI-5 name
 * char-picker). Button callbacks only mark state;
 * tick() rebuilds. Picker and editor views use the
 * port's raw-key mode: UP/DOWN cycle, LEFT/RIGHT
 * move, OK advances/confirms/saves, BACK cancels --
 * the group model cannot express character cycling
 * because UP/DOWN are group navigation (PREV/NEXT).
 *
 * Serial markers (UILV3:) are the verify script's
 * sync points; dumps prove the pixels. A failed
 * flush is counted by the port, never fatal.
 */
#include "ulib/ulib.h"
#include "lv_port.h"
#include "lv_settings.h"
#include "lvgl/lvgl.h"

#define UI_W		240
#define BTN_W		200
#define BTN_H		24
#define BTN_GAP		2
#define LIST_Y0		24

/* Views. */
#define V_LIST		0
#define V_NAME		1
#define V_TZ		2
#define V_PIN		3
#define V_PICK		4
#define V_EDIT		5

/* Picker phases. */
#define P_NONE		0
#define P_LEN_OLD	1
#define P_LEN_NEW	2
#define P_ENT_OLD	3
#define P_ENT_NEW	4
#define P_ENT_CONF	5

/* Timezone options: stored value + display label. The
 * list is curated (web settings still takes free
 * text); the device clock's WIB hardcode is a
 * separate gap (see DEVLOG-UI3), not something this
 * list pretends to fix. */
struct tz_opt {
	const char *val;
	const char *label;
	const char *fkey;	/* focus marker key */
};
static const struct tz_opt tz_opts[] = {
	{ "+0000", "+0000 UTC", "tz+0000" },
	{ "+0700", "+0700 WIB", "tz+0700" },
	{ "+0800", "+0800 WITA", "tz+0800" },
	{ "+0900", "+0900 WIT", "tz+0900" },
	{ "+0600", "+0600", "tz+0600" },
	{ "+1000", "+1000", "tz+1000" },
};
#define NTZ	(sizeof(tz_opts) / sizeof(tz_opts[0]))

static lv_group_t *grp;
static int view;
static int active;
static int pending;		/* view rebuild requested */
static int want_view;		/* view to build on rebuild */

/* Detail state. */
static char cur_name[48];
static char cur_tz[24];
static char detail_msg[40];

/* Picker state. */
static int pick_phase;
static int pick_len;
static int pick_digits[16];
static int pick_cursor;
static int new_len;
static int new_digits[16];
static char pick_msg[40];

/* Name editor state (UI-5). Same charset and key
 * semantics as the old hand-rolled scr_textedit
 * picker: UP/DOWN cycles the character at the
 * cursor, LEFT/RIGHT moves it, LEFT at position 0
 * deletes the last character, RIGHT at the end
 * appends 'A', OK saves, BACK cancels. The buffer
 * cap is EDIT_MAX (the web form's 40-char bound);
 * the web form's form_get() and the shell's setup
 * display truncate to 39, so a 40-char name set
 * here is stored intact and shown in full
 * on-device, but shows truncated in those two
 * legacy display fields (pre-existing limits, not
 * data loss). An empty buffer cannot be saved
 * (the len-0 lfs write edge is avoided); BACK
 * cancels instead. */
#define EDIT_MAX	40
static const char edit_charset[] =
	" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!.,:-?";
static char edit_buf[EDIT_MAX + 1];
static int edit_len;
static int edit_pos;		/* 0..len (len = append) */
static char edit_msg[40];

/* --- tiny string helpers (no printf in uiapp) --- */

static unsigned
str_len(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	unsigned i = 0;
	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == b[i];
}

static void
str_copy(char *d, const char *s, unsigned max)
{
	unsigned i = 0;
	while (s[i] && i + 1u < max) {
		d[i] = s[i];
		i++;
	}
	d[i] = 0;
}

/* --- syscall wrappers (mirror user/sh.c patterns) --- */

static int
webui_call(unsigned sub, char *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #103\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (sub), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

static int
pin_present(void)
{
	return webui_call(3u, (char *)0, 0u);
}

static int
pin_verify(const char *pin, unsigned len)
{
	return webui_call(4u, (char *)pin, len);
}

static int
pin_set(const char *pin, unsigned len)
{
	return webui_call(2u, (char *)pin, len);
}

/* --- cfg reads (same files as web/shell) --- */

static void
read_cfg(void)
{
	int n;

	cur_name[0] = 0;
	n = uilvgl_lfs_read("cfg-name", cur_name, sizeof(cur_name) - 1u);
	if (n > 0)
		cur_name[n] = 0;
	else
		cur_name[0] = 0;
	cur_tz[0] = 0;
	n = uilvgl_lfs_read("cfg-tz", cur_tz, sizeof(cur_tz) - 1u);
	if (n > 0)
		cur_tz[n] = 0;
	else
		cur_tz[0] = 0;
}

/* --- view builders (group emptied + screen cleaned
 * first, exactly like the shell's builders) --- */

static void
focus_cb(lv_event_t *e)
{
	puts("UILV3: focus ");
	puts((const char *)lv_event_get_user_data(e));
	puts("\n");
}

static lv_obj_t *
mk_button(lv_obj_t *scr, int idx, const char *label, const char *key,
	  void (*cb)(lv_event_t *), void *ud)
{
	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_t *lbl;
	lv_obj_set_size(btn, BTN_W, BTN_H);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0,
		     LIST_Y0 + idx * (BTN_H + BTN_GAP));
	lv_obj_set_style_pad_top(btn, 2, 0);
	lv_obj_set_style_pad_bottom(btn, 2, 0);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, label);
	lv_obj_center(lbl);
	lv_group_add_obj(grp, btn);
	if (key)
		lv_obj_add_event_cb(btn, focus_cb, LV_EVENT_FOCUSED,
				    (void *)key);
	if (cb)
		lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
	return btn;
}

static void
goto_cb(lv_event_t *e)
{
	/* Button callbacks only mark; tick() rebuilds. */
	want_view = (int)(long)lv_event_get_user_data(e);
	pending = 1;
}

static void
build_list(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *first;

	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	first = mk_button(scr, 0, "Device name", "name", goto_cb,
			  (void *)(long)V_NAME);
	mk_button(scr, 1, "Timezone", "tz", goto_cb, (void *)(long)V_TZ);
	mk_button(scr, 2, "PIN", "pin", goto_cb, (void *)(long)V_PIN);
	mk_button(scr, 3, "BACK", "setback", goto_cb,
		  (void *)(long)-1);
	lv_group_focus_obj(first);
	view = V_LIST;
	puts("UILV3: settings list\n");
}

static lv_obj_t *
mk_line(lv_obj_t *scr, const char *text, int y)
{
	lv_obj_t *t = lv_label_create(scr);
	lv_label_set_text(t, text);
	lv_obj_align(t, LV_ALIGN_TOP_MID, 0, y);
	return t;
}

static void
back_list_cb(lv_event_t *e)
{
	(void)e;
	want_view = V_LIST;
	pending = 1;
}

/* Forward: the editor lives below the view builders. */
static void edit_enter_cb(lv_event_t *e);

static void
build_name(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *btn, *first;

	read_cfg();
	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	mk_line(scr, "Device name", 30);
	if (cur_name[0])
		mk_line(scr, cur_name, 62);
	else
		mk_line(scr, "(unset)", 62);
	/* UI-5: the value is editable now (char-picker
	 * editor, same pattern as the PIN picker).
	 * Focus starts on Edit (the primary action),
	 * matching the PIN detail's Change/Set focus. */
	first = mk_button(scr, 0, "Edit name", "editname",
			  edit_enter_cb, NULL);
	/* mk_button places by LIST_Y0 pitch; move the
	 * buttons to the detail layout rows. */
	lv_obj_align(first, LV_ALIGN_TOP_MID, 0, 100);
	btn = mk_button(scr, 0, "BACK", "nameback", back_list_cb, NULL);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 130);
	lv_group_focus_obj(first);
	view = V_NAME;
	puts("UILV3: detail name\n");
}

static int
tz_index(const char *val)
{
	unsigned i;
	for (i = 0; i < NTZ; i++)
		if (str_eq(val, tz_opts[i].val))
			return (int)i;
	return 0;
}

static void
tz_save_cb(lv_event_t *e)
{
	const struct tz_opt *o =
	    (const struct tz_opt *)lv_event_get_user_data(e);
	if (uilvgl_lfs_write("cfg-tz", o->val, str_len(o->val)) == 0) {
		read_cfg();
		puts("UILV3: tz saved ");
		puts(o->val);
		puts("\n");
		/* Rebuild the detail so the title shows the
		 * new current value. */
		want_view = V_TZ;
		pending = 1;
	}
}

static void
build_tz(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *title, *btn, *first = NULL;
	char tbuf[40];
	unsigned i;

	read_cfg();
	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	str_copy(tbuf, "Timezone: ", sizeof(tbuf));
	{
		unsigned n = str_len(tbuf), j = 0;
		while (cur_tz[j] && n + 1u < sizeof(tbuf)) {
			tbuf[n++] = cur_tz[j++];
		}
		tbuf[n] = 0;
	}
	title = mk_line(scr, tbuf[0] ? tbuf : "Timezone", 24);
	(void)title;
	for (i = 0; i < NTZ; i++) {
		btn = lv_button_create(scr);
		lv_obj_set_size(btn, BTN_W, 20);
		lv_obj_align(btn, LV_ALIGN_TOP_MID, 0,
			     48 + (int)i * 22);
		lv_obj_set_style_pad_top(btn, 1, 0);
		lv_obj_set_style_pad_bottom(btn, 1, 0);
		{
			lv_obj_t *lbl = lv_label_create(btn);
			lv_label_set_text(lbl, tz_opts[i].label);
			lv_obj_center(lbl);
		}
		lv_group_add_obj(grp, btn);
		lv_obj_add_event_cb(btn, focus_cb, LV_EVENT_FOCUSED,
				    (void *)tz_opts[i].fkey);
		lv_obj_add_event_cb(btn, tz_save_cb, LV_EVENT_CLICKED,
				    (void *)&tz_opts[i]);
		if ((int)i == tz_index(cur_tz))
			first = btn;
	}
	/* BACK below the options. */
	btn = mk_button(scr, 0, "BACK", "tzback", back_list_cb, NULL);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 196);
	if (first)
		lv_group_focus_obj(first);
	view = V_TZ;
	puts("UILV3: detail tz\n");
}

static void
pin_change_cb(lv_event_t *e)
{
	(void)e;
	/* Enter the picker: length select first, old
	 * PIN first when one is set. */
	pick_phase = pin_present() > 0 ? P_LEN_OLD : P_LEN_NEW;
	pick_len = 4;
	pick_cursor = 0;
	pick_msg[0] = 0;
	detail_msg[0] = 0;
	{
		int i;
		for (i = 0; i < 16; i++)
			pick_digits[i] = 0;
	}
	want_view = V_PICK;
	pending = 1;
}

static void
build_pin(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *btn, *first;
	int present = pin_present() > 0;

	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	mk_line(scr, "PIN", 30);
	mk_line(scr, present ? "PIN: Set" : "PIN: Not set", 60);
	if (detail_msg[0])
		mk_line(scr, detail_msg, 82);
	first = mk_button(scr, 0, present ? "Change PIN" : "Set PIN",
			  "changepin", pin_change_cb, NULL);
	lv_obj_align(first, LV_ALIGN_TOP_MID, 0, 100);
	btn = mk_button(scr, 0, "BACK", "pinback", back_list_cb, NULL);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 140);
	lv_group_focus_obj(first);
	view = V_PIN;
	puts("UILV3: detail pin\n");
}

/* --- picker rendering --- */

static void
build_pick_len(void)
{
	lv_obj_t *scr = lv_screen_active();
	char b[24];

	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	mk_line(scr, pick_phase == P_LEN_OLD ?
	    "Enter old PIN: length" : "New PIN: length", 30);
	b[0] = 'L';
	b[1] = 'e';
	b[2] = 'n';
	b[3] = 'g';
	b[4] = 't';
	b[5] = 'h';
	b[6] = ':';
	b[7] = ' ';
	if (pick_len >= 10) {
		b[8] = (char)('0' + pick_len / 10);
		b[9] = (char)('0' + pick_len % 10);
		b[10] = 0;
	} else {
		b[8] = (char)('0' + pick_len);
		b[9] = 0;
	}
	mk_line(scr, b, 70);
	mk_line(scr, "UP/DN: change OK: confirm", 100);
	mk_line(scr, "BACK: cancel", 122);
	if (pick_msg[0])
		mk_line(scr, pick_msg, 150);
	view = V_PICK;
	puts(pick_phase == P_LEN_OLD ? "UILV3: pin len old\n" :
	    "UILV3: pin len new\n");
}

static void
build_pick_entry(void)
{
	lv_obj_t *scr = lv_screen_active();
	char mask[20], pos[24];
	int i;

	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	mk_line(scr, pick_phase == P_ENT_OLD ? "Enter old PIN" :
	    pick_phase == P_ENT_NEW ? "Enter new PIN" :
	    "Confirm new PIN", 30);
	for (i = 0; i < pick_len; i++)
		mask[i] = (i == pick_cursor) ? '_' : '*';
	mask[pick_len] = 0;
	mk_line(scr, mask, 70);
	/* Cursor + current digit, so the picker is
	 * usable without unmasking the whole PIN. */
	pos[0] = 'P';
	pos[1] = 'O';
	pos[2] = 'S';
	pos[3] = ' ';
	{
		/* No leading zeros: "POS 2/4 DIGIT 3". */
		int n = 4, c = pick_cursor + 1;
		if (c >= 10)
			pos[n++] = (char)('0' + c / 10);
		pos[n++] = (char)('0' + c % 10);
		pos[n++] = '/';
		if (pick_len >= 10)
			pos[n++] = (char)('0' + pick_len / 10);
		pos[n++] = (char)('0' + pick_len % 10);
		pos[n++] = ' ';
		pos[n++] = 'D';
		pos[n++] = 'I';
		pos[n++] = 'G';
		pos[n++] = 'I';
		pos[n++] = 'T';
		pos[n++] = ' ';
		pos[n++] = (char)('0' + pick_digits[pick_cursor]);
		pos[n] = 0;
	}
	mk_line(scr, pos, 100);
	mk_line(scr, "UP/DN: digit L/R: move", 130);
	mk_line(scr, "OK: next BACK: cancel", 152);
	if (pick_msg[0])
		mk_line(scr, pick_msg, 180);
	view = V_PICK;
	puts(pick_phase == P_ENT_OLD ? "UILV3: pin entry old\n" :
	    pick_phase == P_ENT_NEW ? "UILV3: pin entry new\n" :
	    "UILV3: pin entry confirm\n");
}

static void
build_pick(void)
{
	if (pick_phase == P_LEN_OLD || pick_phase == P_LEN_NEW)
		build_pick_len();
	else
		build_pick_entry();
}

/* --- picker transitions --- */

static void
digits_to_pin(char *out)
{
	int i;
	for (i = 0; i < pick_len; i++)
		out[i] = (char)('0' + pick_digits[i]);
	out[pick_len] = 0;
}

static void
reset_digits(void)
{
	int i;
	for (i = 0; i < 16; i++)
		pick_digits[i] = 0;
	pick_cursor = 0;
}

static void
to_pin_detail(const char *msg)
{
	str_copy(detail_msg, msg, sizeof(detail_msg));
	uilvgl_raw_keys(0);
	pick_phase = P_NONE;
	want_view = V_PIN;
	pending = 1;
}

static void
pick_confirm_old(void)
{
	char pin[20];
	int r;

	digits_to_pin(pin);
	r = pin_verify(pin, (unsigned)pick_len);
	if (r == 1) {
		puts("UILV3: pin verified\n");
		pick_phase = P_LEN_NEW;
		pick_len = 4;
		reset_digits();
		pick_msg[0] = 0;
		want_view = V_PICK;
		pending = 1;
	} else if (r == -1) {
		puts("UILV3: pin locked\n");
		to_pin_detail("PIN locked - wait, then retry");
	} else {
		puts("UILV3: pin wrong\n");
		to_pin_detail("Wrong PIN - try again");
	}
}

static void
pick_confirm_new(void)
{
	int i;
	for (i = 0; i < pick_len; i++)
		new_digits[i] = pick_digits[i];
	new_len = pick_len;
	pick_phase = P_ENT_CONF;
	reset_digits();
	pick_len = new_len;
	pick_msg[0] = 0;
	want_view = V_PICK;
	pending = 1;
}

static void
pick_confirm_confirm(void)
{
	int i, same = 1;
	char pin[20];

	if (pick_len != new_len)
		same = 0;
	else {
		for (i = 0; i < pick_len; i++)
			if (pick_digits[i] != new_digits[i])
				same = 0;
	}
	if (!same) {
		puts("UILV3: pin mismatch\n");
		str_copy(pick_msg, "Mismatch - try again",
			 sizeof(pick_msg));
		pick_phase = P_ENT_NEW;
		pick_len = new_len;
		reset_digits();
		want_view = V_PICK;
		pending = 1;
		return;
	}
	digits_to_pin(pin);
	if (pin_set(pin, (unsigned)pick_len) == 0) {
		puts("UILV3: pin changed\n");
		to_pin_detail("PIN changed");
	} else {
		to_pin_detail("Set failed - try again");
	}
}

static void
pick_key(int ev)
{
	switch (pick_phase) {
	case P_LEN_OLD:
	case P_LEN_NEW:
		switch (ev) {
		case EV_UP:
			if (pick_len < 16)
				pick_len++;
			break;
		case EV_DOWN:
			if (pick_len > 4)
				pick_len--;
			break;
		case EV_OK:
			pick_phase = (pick_phase == P_LEN_OLD) ?
			    P_ENT_OLD : P_ENT_NEW;
			reset_digits();
			pick_msg[0] = 0;
			break;
		case EV_BACK:
			puts("UILV3: pin cancel\n");
			to_pin_detail("");
			return;
		default:
			return;
		}
		break;
	case P_ENT_OLD:
	case P_ENT_NEW:
	case P_ENT_CONF:
		switch (ev) {
		case EV_UP:
			pick_digits[pick_cursor] =
			    (pick_digits[pick_cursor] + 1) % 10;
			break;
		case EV_DOWN:
			pick_digits[pick_cursor] =
			    (pick_digits[pick_cursor] + 9) % 10;
			break;
		case EV_LEFT:
			if (pick_cursor > 0)
				pick_cursor--;
			break;
		case EV_RIGHT:
			if (pick_cursor < pick_len - 1)
				pick_cursor++;
			break;
		case EV_OK:
			if (pick_cursor < pick_len - 1) {
				pick_cursor++;
				break;
			}
			/* Last position confirms. */
			if (pick_phase == P_ENT_OLD)
				pick_confirm_old();
			else if (pick_phase == P_ENT_NEW)
				pick_confirm_new();
			else
				pick_confirm_confirm();
			return;
		case EV_BACK:
			puts("UILV3: pin cancel\n");
			to_pin_detail("");
			return;
		default:
			return;
		}
		break;
	default:
		return;
	}
	want_view = V_PICK;
	pending = 1;
}

/* --- name editor (UI-5) --- */

static int
edit_ch_count(void)
{
	int i = 0;
	while (edit_charset[i])
		i++;
	return i;
}

static int
edit_ch_idx(char c)
{
	int i = 0;
	while (edit_charset[i]) {
		if (edit_charset[i] == c)
			return i;
		i++;
	}
	return 0;
}

static void
edit_cycle(int dir)
{
	int n = edit_ch_count(), i;

	if (edit_pos >= edit_len) {
		/* Append position: start a new character
		 * (space) and cycle it, exactly like the
		 * old picker. Blocked at the cap. */
		if (edit_len >= EDIT_MAX)
			return;
		edit_buf[edit_len] = edit_charset[0];
		edit_buf[edit_len + 1] = 0;
		edit_pos = edit_len;
		edit_len++;
	}
	i = edit_ch_idx(edit_buf[edit_pos]);
	i = (i + dir + n) % n;
	edit_buf[edit_pos] = edit_charset[i];
}

static void
to_name_detail(void)
{
	uilvgl_raw_keys(0);
	want_view = V_NAME;
	pending = 1;
}

static void
edit_key(int ev)
{
	switch (ev) {
	case EV_UP:
		edit_cycle(1);
		break;
	case EV_DOWN:
		edit_cycle(-1);
		break;
	case EV_LEFT:
		if (edit_pos > 0) {
			edit_pos--;
		} else if (edit_len > 0) {
			/* At position 0: delete the last
			 * character (old picker semantics)
			 * and land at the end. */
			edit_len--;
			edit_buf[edit_len] = 0;
			edit_pos = edit_len;
		}
		break;
	case EV_RIGHT:
		if (edit_pos < edit_len) {
			edit_pos++;
		} else if (edit_len < EDIT_MAX) {
			/* At the end: append 'A' (old
			 * picker semantics) and land past
			 * it, back in append position. */
			edit_buf[edit_len] = edit_charset[1];
			edit_len++;
			edit_buf[edit_len] = 0;
			edit_pos = edit_len;
		}
		break;
	case EV_OK:
		if (edit_len == 0) {
			str_copy(edit_msg, "Empty - not saved",
				 sizeof(edit_msg));
			break;
		}
		if (uilvgl_lfs_write("cfg-name", edit_buf,
		    (unsigned)edit_len) == 0) {
			puts("UILV5: edit saved ");
			puts(edit_buf);
			puts("\n");
			edit_msg[0] = 0;
			to_name_detail();
			return;
		}
		str_copy(edit_msg, "Save failed - try again",
			 sizeof(edit_msg));
		break;
	case EV_BACK:
		puts("UILV5: edit cancel\n");
		edit_msg[0] = 0;
		to_name_detail();
		return;
	default:
		return;
	}
	want_view = V_EDIT;
	pending = 1;
}

static void
build_edit(void)
{
	lv_obj_t *scr = lv_screen_active();
	char pos[32];
	int n = 0, c;

	lv_group_remove_all_objs(grp);
	lv_obj_clean(scr);
	mk_line(scr, "Edit device name", 24);
	if (edit_len > 0)
		mk_line(scr, edit_buf, 56);
	else
		mk_line(scr, "(empty)", 56);
	/* Cursor + current character, so the editor is
	 * usable without a pixel cursor: "POS c/len
	 * CHAR x", '_' in append position. */
	pos[n++] = 'P'; pos[n++] = 'O'; pos[n++] = 'S'; pos[n++] = ' ';
	c = edit_pos + 1;
	if (c >= 10)
		pos[n++] = (char)('0' + c / 10);
	pos[n++] = (char)('0' + c % 10);
	pos[n++] = '/';
	if (edit_len >= 10)
		pos[n++] = (char)('0' + edit_len / 10);
	pos[n++] = (char)('0' + edit_len % 10);
	pos[n++] = ' ';
	pos[n++] = 'C'; pos[n++] = 'H'; pos[n++] = 'A'; pos[n++] = 'R';
	pos[n++] = ' ';
	if (edit_pos < edit_len)
		pos[n++] = edit_buf[edit_pos];
	else
		pos[n++] = '_';
	pos[n] = 0;
	mk_line(scr, pos, 96);
	mk_line(scr, "UP/DN: char L/R: move", 128);
	mk_line(scr, "LEFT@0: del RIGHT@end: add", 150);
	mk_line(scr, "OK: save BACK: cancel", 172);
	if (edit_msg[0])
		mk_line(scr, edit_msg, 196);
	view = V_EDIT;
	puts("UILV5: edit name\n");
}

static void
edit_enter_cb(lv_event_t *e)
{
	int i;

	(void)e;
	/* Start from the stored value (clamped to the
	 * cap), cursor at the end, like the old picker. */
	for (i = 0; i < EDIT_MAX && cur_name[i]; i++)
		edit_buf[i] = cur_name[i];
	edit_buf[i] = 0;
	edit_len = i;
	edit_pos = edit_len;
	edit_msg[0] = 0;
	want_view = V_EDIT;
	pending = 1;
}

/* --- module API --- */

void
lvset_enter(lv_group_t *g)
{
	grp = g;
	active = 1;
	detail_msg[0] = 0;
	pick_phase = P_NONE;
	build_list();
}

int
lvset_tick(void)
{
	int ev, exited = 0;

	if (pending) {
		pending = 0;
		switch (want_view) {
		case V_LIST:
			build_list();
			break;
		case V_NAME:
			build_name();
			break;
		case V_TZ:
			build_tz();
			break;
		case V_PIN:
			build_pin();
			break;
		case V_PICK:
			build_pick();
			/* Picker views own the EV queue. */
			uilvgl_raw_keys(1);
			break;
		case V_EDIT:
			build_edit();
			/* The editor owns the EV queue, like
			 * the picker. */
			uilvgl_raw_keys(1);
			break;
		default:
			/* -1 from the list BACK button: the
			 * caller rebuilds the shell menu. */
			exited = 1;
			break;
		}
	}
	/* Picker views own the EV queue via raw mode. */
	if (view == V_PICK && pick_phase != P_NONE) {
		ev = uilvgl_take_key();
		if (ev != EV_NONE)
			pick_key(ev);
	} else if (view == V_EDIT) {
		ev = uilvgl_take_key();
		if (ev != EV_NONE)
			edit_key(ev);
	}
	return exited;
}

int
lvset_on_back(void)
{
	if (view == V_PICK || view == V_EDIT) {
		/* Picker/editor already consumed BACK as
		 * cancel in tick(); a group-mode BACK here
		 * cannot happen (raw mode never sets the
		 * edge), so treat it as consumed. */
		return 1;
	}
	if (view == V_LIST)
		return 0;
	/* Detail -> list. */
	want_view = V_LIST;
	pending = 1;
	/* Tick runs before the loop's next BACK check,
	 * so the rebuild lands this turn. */
	lvset_tick();
	return 1;
}

int
lvset_active(void)
{
	return active;
}

void
lvset_exit(void)
{
	uilvgl_raw_keys(0);
	active = 0;
	pick_phase = P_NONE;
	edit_len = 0;
	edit_pos = 0;
	edit_buf[0] = 0;
}
