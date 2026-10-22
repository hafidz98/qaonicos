/*
 * webui.h -- ONB-1: state for the web settings UI (PIN, session,
 * web shell bridge). Implemented in webui.c; http.c is the
 * router, user.c exposes the userspace half as syscall 103.
 * Design: docs/rencana-onboarding/PLAN-ONB1-ONB2.md.
 */
#ifndef QAONIC_WEBUI_H
#define QAONIC_WEBUI_H

/* Config files (littlefs, shared with the shell's `setup`). */
int	webui_cfg_read(const char *name, char *buf, unsigned int max);
int	webui_cfg_write(const char *name, const char *val,
	    unsigned int len);
int	webui_cfg_clear(const char *name);

/* Device PIN: stored as a salted SHA-256 hash in cfg-pin. */
int	webui_pin_present(void);
int	webui_pin_set(const char *pin, unsigned int len);
int	webui_pin_verify(const char *pin, unsigned int len);

/* Single admin session (cookie token, 30 min idle expiry). */
int	webui_session_login(char *tok_out /* >=33 bytes */);
int	webui_session_check(const char *tok, unsigned int len);

/* Web shell bridge (D-ONB-5: off by default, Settings toggle,
 * 15 min idle auto-off). */
int	webui_shell_enabled(void);
void	webui_shell_set_enabled(int on);
int	webui_shell_submit(const char *cmd, unsigned int len);
int	webui_shell_result(char *buf, unsigned int max);

/* Userspace half (called from the syscall 103 handler). */
int	webui_shell_take(char *buf, unsigned int max);
int	webui_shell_post(const char *buf, unsigned int len);

#endif
