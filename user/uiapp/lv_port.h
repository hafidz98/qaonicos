/*
 * lv_port.h - LVGL port for QaonicOS uiapp (UI-1).
 *
 * The port owns exactly the three callbacks the LVGL
 * research names: rect flush via SYS_DISPLAY_FLUSH,
 * tick from sys_uptime(), keypad input from the EV
 * queue. The smoke screen is marker-gated (see
 * uilvgl_smoke_enabled) so the normal menu is untouched
 * unless /data/ui1-test reads "1".
 */
#ifndef UIAPP_LV_PORT_H
#define UIAPP_LV_PORT_H

/* Marker-gated: 1 when the LVGL smoke screen should run
 * on the next token grant, else 0 (normal menu). Safe to
 * call before the token is held; a failed lfs mount
 * means "not enabled", never a uiapp failure. */
int	uilvgl_smoke_enabled(void);

/* Run the LVGL smoke screen until BACK, token loss, or
 * 30 s without input. Returns to the caller, which owns
 * the display-token release (as the menu loop does). */
void	uilvgl_run(void);

#endif /* UIAPP_LV_PORT_H */
