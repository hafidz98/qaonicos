/*
 * lv_settings.h - LVGL Settings screen (UI-3).
 *
 * The shell's Settings entry is a real submenu, not
 * the UI-2 placeholder: Device name, Timezone, PIN.
 * Device name and timezone read the same littlefs
 * files the web settings page and the shell setup
 * command use (cfg-name, cfg-tz) -- one source of
 * truth, never a parallel copy. The PIN goes through
 * the kernel PIN code (syscall 103: present/set, and
 * the UI-3 verify sub) so the SHA-256 hash, the
 * constant-time compare, and the throttle apply
 * exactly as they do for the web login.
 *
 * Editing (UI-3): timezone is a fixed option list
 * (keypad-selectable, no text entry needed) and the
 * PIN is a digit-picker (digits only, 4..16
 * positions). The device name is display-only
 * on-device (UI-7: editing is web-config only; the
 * UI-5 char-picker editor was removed). Non-numeric
 * PINs can only change via the web UI.
 *
 * The module is driven by the shell loop: enter()
 * builds the list, tick() handles pending rebuilds
 * and (in picker views) raw keys, on_back() walks
 * one level up (0 = exited to the shell menu).
 */
#ifndef UIAPP_LV_SETTINGS_H
#define UIAPP_LV_SETTINGS_H

#include "lvgl/lvgl.h"

/* Build the settings list and mark settings active. */
void	lvset_enter(lv_group_t *grp);

/* One loop turn while settings are active: pending
 * view rebuilds, plus raw-key polling in picker
 * views. Safe to call when no rebuild is pending.
 * Returns 1 when the list BACK button was pressed
 * (settings exited; the caller rebuilds the shell
 * menu), else 0. */
int	lvset_tick(void);

/* BACK while settings are active: 1 = consumed
 * (still in settings, view rebuilt), 0 = exited
 * (the caller rebuilds the shell menu). */
int	lvset_on_back(void);

/* 1 while a settings view (list, detail, picker) is
 * showing, else 0. */
int	lvset_active(void);

/* Defensive teardown: raw mode off, inactive. The
 * shell loop calls this on release/idle exit. */
void	lvset_exit(void);

#endif /* UIAPP_LV_SETTINGS_H */
