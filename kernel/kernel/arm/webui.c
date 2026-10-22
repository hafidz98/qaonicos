/*
 * webui.c -- ONB-1: web settings UI state (PIN, session, web
 * shell bridge). See webui.h and
 * docs/rencana-onboarding/PLAN-ONB1-ONB2.md.
 *
 * PIN storage is a salted SHA-256 hash in the littlefs file
 * cfg-pin -- the ONB-1 baseline; B5.5 (credential store)
 * hardens it later. Session tokens are timer-grade entropy
 * until the RV1103 TRNG lands (known debt, same as TLS).
 */

#include "webui.h"
#include "updverify.h"

extern unsigned int	arm_timer_ticks(void);
extern int		lfs_port_read(const char *, unsigned char *,
			    unsigned int);
extern int		lfs_port_write(const char *, const unsigned char *,
			    unsigned int);
extern int		bootstate_info(unsigned int *, unsigned int *,
			    unsigned int *, unsigned int *);

static unsigned int
now_ms(void)
{
	return arm_timer_ticks() * 10u;
}

static unsigned int
slen(const char *s)
{
	unsigned int n = 0;
	while (s[n])
		n++;
	return n;
}

static int
seq(const char *a, const char *b, unsigned int n)
{
	unsigned int i;
	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* --- config files --- */

int
webui_cfg_read(const char *name, char *buf, unsigned int max)
{
	int n = lfs_port_read(name, (unsigned char *)buf, max - 1u);
	if (n < 0) {
		buf[0] = 0;
		return -1;
	}
	buf[n] = 0;
	return n;
}

int
webui_cfg_write(const char *name, const char *val, unsigned int len)
{
	return lfs_port_write(name, (const unsigned char *)val, len);
}

int
webui_cfg_clear(const char *name)
{
	/* littlefs port has no unlink; an empty file reads as unset. */
	return lfs_port_write(name, (const unsigned char *)"", 0u);
}

/* --- PIN --- */

#define PIN_FILE	"cfg-pin"
#define PIN_PREFIX	"qaon-pin-v1:"

static void
pin_hash_hex(const char *pin, unsigned int len, char hex[65])
{
	static const char *hx = "0123456789abcdef";
	unsigned char dig[32];
	unsigned char msg[64];
	unsigned int i, n = 0;
	const char *p = PIN_PREFIX;

	while (*p && n < sizeof(msg))
		msg[n++] = (unsigned char)*p++;
	for (i = 0; i < len && n < sizeof(msg); i++)
		msg[n++] = (unsigned char)pin[i];
	upd_sha256(msg, n, dig);
	for (i = 0; i < 32u; i++) {
		hex[i * 2u] = hx[dig[i] >> 4];
		hex[i * 2u + 1u] = hx[dig[i] & 15u];
	}
	hex[64] = 0;
}

int
webui_pin_present(void)
{
	char buf[80];
	return webui_cfg_read(PIN_FILE, buf, sizeof(buf)) >= 64;
}

int
webui_pin_set(const char *pin, unsigned int len)
{
	char hex[65];

	if (len < 4u || len > 16u)
		return -1;
	pin_hash_hex(pin, len, hex);
	return webui_cfg_write(PIN_FILE, hex, 64u);
}

int
webui_pin_verify(const char *pin, unsigned int len)
{
	char stored[80], hex[65];
	int n = webui_cfg_read(PIN_FILE, stored, sizeof(stored));

	if (n < 64 || len < 4u || len > 16u)
		return 0;
	pin_hash_hex(pin, len, hex);
	return seq(stored, hex, 64u);
}

/* --- session --- */

#define SESSION_IDLE_MS	(30u * 60u * 1000u)

static char		sess_tok[33];
static unsigned int	sess_last_ms;
static int		sess_valid;

int
webui_session_login(char *tok_out)
{
	unsigned char dig[32];
	unsigned char msg[96];
	char stored[80];
	unsigned int bc = 0, fc = 0, fl = 0, up = 0;
	unsigned int i, n = 0;
	static const char *hx = "0123456789abcdef";
	int sn = webui_cfg_read(PIN_FILE, stored, sizeof(stored));

	if (sn < 64)
		return -1;
	for (i = 0; i < 64u; i++)
		msg[n++] = (unsigned char)stored[i];
	(void)bootstate_info(&bc, &fc, &fl, &up);
	msg[n++] = (unsigned char)(bc & 0xffu);
	msg[n++] = (unsigned char)((bc >> 8) & 0xffu);
	{
		unsigned int t = now_ms();
		msg[n++] = (unsigned char)(t & 0xffu);
		msg[n++] = (unsigned char)((t >> 8) & 0xffu);
		msg[n++] = (unsigned char)((t >> 16) & 0xffu);
		msg[n++] = (unsigned char)((t >> 24) & 0xffu);
	}
	upd_sha256(msg, n, dig);
	for (i = 0; i < 16u; i++) {
		sess_tok[i * 2u] = hx[dig[i] >> 4];
		sess_tok[i * 2u + 1u] = hx[dig[i] & 15u];
	}
	sess_tok[32] = 0;
	sess_valid = 1;
	sess_last_ms = now_ms();
	for (i = 0; i < 33u; i++)
		tok_out[i] = sess_tok[i];
	return 0;
}

int
webui_session_check(const char *tok, unsigned int len)
{
	if (!sess_valid || len != 32u)
		return 0;
	if (now_ms() - sess_last_ms > SESSION_IDLE_MS) {
		sess_valid = 0;
		return 0;
	}
	if (!seq(tok, sess_tok, 32u))
		return 0;
	sess_last_ms = now_ms();
	return 1;
}

/* --- web shell bridge --- */

#define SHELL_IDLE_MS	(15u * 60u * 1000u)
#define WS_CMD_MAX	160u
#define WS_OUT_MAX	4096u

/* states: 0 idle, 1 pending (queued for sh), 2 running (sh has
 * it), 3 done (result waiting for the web side) */
static volatile unsigned int ws_state;
static char		ws_cmd[WS_CMD_MAX];
static char		ws_out[WS_OUT_MAX];
static unsigned int	ws_out_len;
static int		ws_enabled_flag;
static unsigned int	ws_last_ms;

int
webui_shell_enabled(void)
{
	if (ws_enabled_flag &&
	    now_ms() - ws_last_ms > SHELL_IDLE_MS)
		ws_enabled_flag = 0;
	return ws_enabled_flag;
}

void
webui_shell_set_enabled(int on)
{
	ws_enabled_flag = on != 0;
	ws_last_ms = now_ms();
	if (!on && ws_state == 1u)
		ws_state = 0;	/* drop a queued, unclaimed command */
}

int
webui_shell_submit(const char *cmd, unsigned int len)
{
	unsigned int i;

	if (!webui_shell_enabled() || ws_state == 1u || ws_state == 2u)
		return -1;
	if (len == 0u || len >= WS_CMD_MAX)
		return -1;
	for (i = 0; i < len; i++)
		ws_cmd[i] = cmd[i];
	ws_cmd[len] = 0;
	ws_out_len = 0;
	ws_state = 1;
	ws_last_ms = now_ms();
	return 0;
}

int
webui_shell_result(char *buf, unsigned int max)
{
	unsigned int i, n;

	if (ws_state != 3u)
		return -1;
	n = ws_out_len < max - 1u ? ws_out_len : max - 1u;
	for (i = 0; i < n; i++)
		buf[i] = ws_out[i];
	buf[n] = 0;
	ws_state = 0;
	ws_last_ms = now_ms();
	return (int)n;
}

int
webui_shell_take(char *buf, unsigned int max)
{
	unsigned int i, n;

	if (ws_state != 1u)
		return -1;
	n = slen(ws_cmd);
	if (n >= max)
		n = max - 1u;
	for (i = 0; i < n; i++)
		buf[i] = ws_cmd[i];
	buf[n] = 0;
	ws_state = 2;
	ws_last_ms = now_ms();
	return (int)n;
}

int
webui_shell_post(const char *buf, unsigned int len)
{
	unsigned int i;

	if (ws_state != 2u)
		return -1;
	if (len >= WS_OUT_MAX)
		len = WS_OUT_MAX - 1u;
	for (i = 0; i < len; i++)
		ws_out[i] = buf[i];
	ws_out_len = len;
	ws_state = 3;
	ws_last_ms = now_ms();
	return 0;
}
