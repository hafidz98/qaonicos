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

/* Milliseconds of the last keypad press (any key),
 * for idle tracking. Set by the keypad callback and
 * initialised by begin(). */
unsigned uilvgl_last_input_ms(void);

/* Count of failed SYS_DISPLAY_FLUSH calls since the
 * current port session began. */
unsigned uilvgl_flush_failures(void);

#endif /* UIAPP_LV_PORT_H */
