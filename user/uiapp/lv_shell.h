/*
 * lv_shell.h - LVGL shell: status bar + menu (UI-2).
 *
 * The shell is the normal uiapp menu: a status bar on
 * the top layer (live clock, dim radio icons, battery
 * outline — the honest-state rule: no radio state is
 * claimed under QEMU) above a keypad-navigable menu.
 * Wired entries return their action so main.c can run
 * the existing hand-rolled screen under the same token
 * grant; placeholder entries (targets of UI-3/UI-4)
 * show an honest "coming in UI-x" screen instead of
 * fake functionality. BACK at the menu root, token
 * loss, or 30 s idle all return SHELL_RELEASE.
 */
#ifndef UIAPP_LV_SHELL_H
#define UIAPP_LV_SHELL_H

#define SHELL_RELEASE	0	/* release the display token */
#define SHELL_WIFI	1	/* run the existing Wi-Fi screen */
#define SHELL_BLE	2	/* run the existing Bluetooth screen */
#define SHELL_LLM	3	/* run the existing LLM screen */
#define SHELL_PASSKEY	4	/* run the existing Passkey screen */
#define SHELL_POWER	5	/* run the existing Power screen */

/* Run the shell until it returns an action. The caller
 * owns the display-token release, as the menu loop does. */
int	uilvgl_shell_run(void);

#endif /* UIAPP_LV_SHELL_H */
