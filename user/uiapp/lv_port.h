/*
 * lv_port.h - LVGL port for QaonicOS uiapp (UI-1 port,
 * UI-2 shell lifecycle).
 *
 * The port owns exactly the three callbacks the LVGL
 * research names: rect flush via SYS_DISPLAY_FLUSH,
 * tick from sys_uptime(), keypad input from the EV
 * queue. The smoke screen is marker-gated (see
 * uilvgl_smoke_enabled) so UI1-VERIFY keeps working;
 * the normal menu is the LVGL shell (lv_shell.c).
 *
 * Port lifecycle for screens (smoke and shell share
 * it): uilvgl_shell_begin() creates display + keypad
 * indev + focus group (lv_init once); uilvgl_shell_end()
 * deletes all three. BACK presses are edges: the
 * keypad callback records them, uilvgl_take_back()
 * consumes one. A failed flush is counted, never
 * fatal (see uilvgl_flush_failures).
 */
#ifndef UIAPP_LV_PORT_H
#define UIAPP_LV_PORT_H

#include "lvgl/lvgl.h"

/* Marker-gated: 1 when the LVGL smoke screen should run
 * on the next token grant, else 0 (normal menu). Safe to
 * call before the token is held; a failed lfs mount
 * means "not enabled", never a uiapp failure. */
int	uilvgl_smoke_enabled(void);

/* Run the LVGL smoke screen until BACK, token loss, or
 * 30 s without input. Returns to the caller, which owns
 * the display-token release (as the menu loop does). */
void	uilvgl_run(void);

/* Shared port lifecycle. begin() returns the focus
 * group (never NULL on success); end() deletes indev,
 * group, and display, in that order. */
lv_group_t *uilvgl_shell_begin(void);
void	uilvgl_shell_end(void);

/* Consume one recorded BACK press (1) or report none
 * (0). The keypad callback records BACK as an edge;
 * whoever owns the current screen decides what BACK
 * means (smoke exits; the shell goes back a level or
 * releases). */
int	uilvgl_take_back(void);

/* Raw-key mode (UI-3): while on, the keypad indev
 * stops draining the EV queue and uilvgl_take_key()
 * returns one raw EV code per call (EV_NONE when the
 * queue holds no key). BACK in raw mode is a plain
 * key and does not set the exit edge. */
void	uilvgl_raw_keys(int on);
int	uilvgl_take_key(void);

/* Littlefs read/write for uiapp screens (syscalls
 * 96/97; returns len/0 or a negative error). The
 * Settings screen reads cfg-name/cfg-tz and writes
 * cfg-tz through these -- the same files the web
 * settings page and the shell setup command use. */
int	uilvgl_lfs_read(const char *path, char *buf, unsigned max);
int	uilvgl_lfs_write(const char *path, const char *buf, unsigned len);

/* Timezone offset in minutes from cfg-tz (UI-4):
 * parsed "+HHMM"/"-HHMM", WIB (+420) on missing or
 * malformed values. Cached (see lv_port.c). */
int	uilvgl_tz_offset_min(void);

/* Milliseconds of the last keypad press (any key),
 * for idle tracking. Set by the keypad callback and
 * initialised by begin(). */
unsigned uilvgl_last_input_ms(void);

/* Count of failed SYS_DISPLAY_FLUSH calls since the
 * current port session began. */
unsigned uilvgl_flush_failures(void);

/* Status-bar icons (UI-7): enables port-painted
 * wifi/bluetooth/battery icons on the top strip and
 * sets their states (1 = on, 0 = off/slashed).
 * Called by the shell when it builds its status bar;
 * the smoke screen never calls it and stays
 * icon-free. QEMU callers pass 0, 0 (no radio). */
void	uilvgl_set_radio_icons(int wifi_on, int bt_on);

#endif /* UIAPP_LV_PORT_H */
