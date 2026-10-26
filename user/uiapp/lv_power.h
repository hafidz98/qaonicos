/*
 * lv_power.h - LVGL Power screen (UI-4).
 *
 * Replaces the Power entry's wiring to the old
 * hand-rolled scr_power screen (a UI-2 coordinator
 * follow-up) with a real LVGL screen offering the
 * same three actions in the same order (Shutdown /
 * Restart / Sleep Now) plus BACK. Safety semantics
 * are scr_power's, preserved exactly: Shutdown and
 * Restart act immediately on OK (SYS_POWEROFF /
 * SYS_RESET, which never return on success); the
 * old screen asked no confirmation, so neither
 * does this one. Sleep Now blanks the display via
 * SYS_DISPLAY_SLEEP(1) and then releases the token,
 * exactly like the old screen. If a power syscall
 * ever returns (platform refused), the screen shows
 * a "refused" status and stays usable instead of
 * the old screen's yield-forever fallback.
 *
 * Driven by the shell loop like lv_settings:
 * enter() builds the screen, tick() returns 1 when
 * BACK was pressed (caller rebuilds the shell
 * menu) or 2 when Sleep Now was pressed (caller
 * releases the token), on_back() returns 0 at the
 * root view.
 */
#ifndef UIAPP_LV_POWER_H
#define UIAPP_LV_POWER_H

#include "lvgl/lvgl.h"

/* Build the power screen and mark it active. */
void	lvpower_enter(lv_group_t *grp);

/* One loop turn while the power screen is active.
 * Returns 1 when BACK was pressed (power exited;
 * the caller rebuilds the shell menu), 2 when
 * Sleep Now was pressed (the caller releases the
 * token), else 0. */
int	lvpower_tick(void);

/* BACK while the power screen is active: 0 =
 * exited (the caller rebuilds the shell menu).
 * The power screen has no sub-views, so BACK
 * always exits. */
int	lvpower_on_back(void);

/* 1 while the power screen is showing, else 0. */
int	lvpower_active(void);

/* Defensive teardown: inactive. The shell loop
 * calls this on release/idle exit. */
void	lvpower_exit(void);

#endif /* UIAPP_LV_POWER_H */
