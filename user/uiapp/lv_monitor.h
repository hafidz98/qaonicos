/*
 * lv_monitor.h - LVGL Monitor screen (UI-4).
 *
 * The shell's Monitor entry is a real screen, not the
 * UI-2/UI-3 placeholder: one label row per value, all
 * from real device data (SYS_STAT 57, bootstate 98
 * sub 0, cfg-name, wall clock + sync state). The
 * honest-state rule applies to every row: a value
 * the kernel cannot provide (CPU: no idle accounting)
 * is shown as "-", never fabricated. Rows refresh on
 * a 500 ms timer; the row texts are also printed to
 * the console on entry (UILV4: mon ...) so a verify
 * script can assert exact values without OCR.
 *
 * Driven by the shell loop like lv_settings: enter()
 * builds the screen, tick() returns 1 when the BACK
 * button was pressed (caller rebuilds the shell
 * menu), on_back() returns 0 at the root view.
 */
#ifndef UIAPP_LV_MONITOR_H
#define UIAPP_LV_MONITOR_H

#include "lvgl/lvgl.h"

/* Build the monitor screen and mark it active. */
void	lvmon_enter(lv_group_t *grp);

/* One loop turn while the monitor is active.
 * Returns 1 when the BACK button was pressed
 * (monitor exited; the caller rebuilds the shell
 * menu), else 0. */
int	lvmon_tick(void);

/* BACK while the monitor is active: 0 = exited
 * (the caller rebuilds the shell menu). The
 * monitor has no sub-views, so BACK always exits. */
int	lvmon_on_back(void);

/* 1 while the monitor screen is showing, else 0. */
int	lvmon_active(void);

/* Defensive teardown: refresh timer deleted,
 * inactive. The shell loop calls this on
 * release/idle exit. */
void	lvmon_exit(void);

#endif /* UIAPP_LV_MONITOR_H */
