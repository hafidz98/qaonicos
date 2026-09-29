/*
 * user/qabot/tools.c -- tool registry v1.
 *
 * Tiap tool: nama, deskripsi, klasifikasi risiko, fungsi eksekusi.
 * Eksekusi menempel langsung ke syscall yang sudah ada.
 */
#include "qabot.h"
#include "../ulib/ulib.h"

static const char *
qb_arg(struct qb_toolcall *tc, const char *key)
{
	unsigned	i, k;

	for (i = 0; i < tc->nargs; i++) {
		for (k = 0; tc->akey[i][k] && key[k] &&
		     tc->akey[i][k] == key[k]; k++)
			;
		if (!tc->akey[i][k] && !key[k])
			return tc->aval[i];
	}
	return 0;
}

static unsigned
qb_atou(const char *s)
{
	unsigned	v = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (unsigned)(*s - '0');
		s++;
	}
	return v;
}

/* get_info: statistik sistem (AMAN). */
static int
tool_get_info(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	struct qaon_stat	st;

	(void)tc;
	if (sys_stat(&st) != 0) {
		qb_snprintf(out, outlen, "error: sys_stat gagal");
		return -1;
	}
	qb_snprintf(out, outlen,
	    "uptime_ms=%u mem=%u/%uKB cpu=%u%%",
	    st.uptime_ms, st.mem_used_kb, st.mem_total_kb, st.cpu_pct);
	return 0;
}

/* get_time: jam dinding (AMAN). */
static int
tool_get_time(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	unsigned	t;

	(void)tc;
	t = sys_time_get();
	if (t == 0)
		qb_snprintf(out, outlen, "time=0 (belum sinkron NTP)");
	else
		qb_snprintf(out, outlen, "time=%u (unix, UTC)", t);
	return 0;
}

/* gpio_read pin=N (AMAN). */
static int
tool_gpio_read(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	const char	*ps;
	int		v;

	ps = qb_arg(tc, "pin");
	if (!ps) {
		qb_snprintf(out, outlen, "error: arg pin hilang");
		return -1;
	}
	v = sys_gpio_get(qb_atou(ps));
	if (v < 0) {
		qb_snprintf(out, outlen, "error: gpio_get pin=%s gagal", ps);
		return -1;
	}
	qb_snprintf(out, outlen, "pin=%s val=%d", ps, v);
	return 0;
}

/* gpio_write pin=N val=0/1 (KONFIRMASI). */
static int
tool_gpio_write(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	const char	*ps, *vs;
	unsigned	pin, val;

	ps = qb_arg(tc, "pin");
	vs = qb_arg(tc, "val");
	if (!ps || !vs) {
		qb_snprintf(out, outlen, "error: arg pin/val hilang");
		return -1;
	}
	pin = qb_atou(ps);
	val = qb_atou(vs) ? 1 : 0;
	if (sys_gpio_set(pin, val) != 0) {
		qb_snprintf(out, outlen, "error: gpio_set pin=%u gagal", pin);
		return -1;
	}
	qb_snprintf(out, outlen, "pin=%u val=%u ok", pin, val);
	return 0;
}

/* Q7: file_read path=P (AMAN). Baca file dari FAT (maks 511 byte). */
static int
tool_file_read(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	const char	*pp;
	static char	buf[512];
	int		n;

	pp = qb_arg(tc, "path");
	if (!pp) {
		qb_snprintf(out, outlen, "error: arg path hilang");
		return -1;
	}
	n = sys_fat_read(pp, buf, sizeof(buf) - 1);
	if (n < 0) {
		qb_snprintf(out, outlen, "error: baca %s gagal", pp);
		return -1;
	}
	buf[n] = 0;
	qb_snprintf(out, outlen, "path=%s bytes=%d", pp, n);
	/* Sambung data (dibatasi outlen). */
	{
		unsigned	i = 0, j = 0;
		while (out[i])
			i++;
		if (i + 7 < outlen) {
			const char	*sep = " data=";
			while (*sep && i + 1 < outlen)
				out[i++] = *sep++;
			while (j < (unsigned)n && i + 1 < outlen)
				out[i++] = buf[j++];
			out[i] = 0;
		}
	}
	return 0;
}

/* Q7: file_write path=P data=D (KONFIRMASI). Tulis file ke FAT. */
static int
tool_file_write(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	const char	*pp, *dp;
	unsigned	dlen;
	int		n;

	pp = qb_arg(tc, "path");
	dp = qb_arg(tc, "data");
	if (!pp || !dp) {
		qb_snprintf(out, outlen, "error: arg path/data hilang");
		return -1;
	}
	for (dlen = 0; dp[dlen]; dlen++)
		;
	n = sys_fat_write(pp, dp, dlen);
	if (n < 0) {
		qb_snprintf(out, outlen, "error: tulis %s gagal", pp);
		return -1;
	}
	qb_snprintf(out, outlen, "path=%s bytes=%d ok", pp, n);
	return 0;
}

/* Q7: file_list path=P (AMAN). Daftar isi direktori FAT. */
static int
tool_file_list(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	const char	*pp;
	static char	buf[512];
	int		n;

	pp = qb_arg(tc, "path");
	if (!pp)
		pp = "/";
	n = sys_readdir(pp, buf, sizeof(buf) - 1);
	if (n < 0) {
		qb_snprintf(out, outlen, "error: list %s gagal", pp);
		return -1;
	}
	buf[sizeof(buf) - 1] = 0;
	qb_snprintf(out, outlen, "path=%s count=%d", pp, n);
	{
		unsigned	i = 0, j = 0;
		while (out[i])
			i++;
		if (i + 8 < outlen) {
			const char	*sep = " files=";
			while (*sep && i + 1 < outlen)
				out[i++] = *sep++;
			while (buf[j] && i + 1 < outlen)
				out[i++] = buf[j++];
			out[i] = 0;
		}
	}
	return 0;
}

/* Q7: sys_uptime (AMAN). Ms sejak boot. */
static int
tool_sys_uptime(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	unsigned	ms;

	(void)tc;
	ms = sys_uptime();
	qb_snprintf(out, outlen, "uptime_ms=%u uptime_s=%u", ms, ms / 1000u);
	return 0;
}

/* Q7: net_status (AMAN). Info TCP/IP. */
static int
tool_net_status(struct qb_toolcall *tc, char *out, unsigned outlen)
{
	int		st;

	(void)tc;
	st = sys_tcp_status();
	/* Jaringan via slirp QEMU: guest 10.0.2.15, gateway/host 10.0.2.2. */
	qb_snprintf(out, outlen, "ip=10.0.2.15 gw=10.0.2.2 tcp_status=%d",
	    st);
	return 0;
}

static const struct qb_tooldef qb_tools[] = {
	{ "get_info",	"statistik sistem (uptime, memori)",
	  QB_RISK_SAFE,		tool_get_info },
	{ "get_time",	"jam dinding; 0 bila belum sinkron NTP",
	  QB_RISK_SAFE,		tool_get_time },
	{ "gpio_read",	"baca pin GPIO (arg: pin=N)",
	  QB_RISK_SAFE,		tool_gpio_read },
	{ "gpio_write",	"tulis pin GPIO (arg: pin=N val=0/1)",
	  QB_RISK_CONFIRM,	tool_gpio_write },
	{ "file_read",	"baca file FAT (arg: path=P)",
	  QB_RISK_SAFE,		tool_file_read },
	{ "file_write",	"tulis file FAT (arg: path=P data=D)",
	  QB_RISK_CONFIRM,	tool_file_write },
	{ "file_list",	"daftar isi direktori FAT (arg: path=P)",
	  QB_RISK_SAFE,		tool_file_list },
	{ "sys_uptime",	"ms sejak boot",
	  QB_RISK_SAFE,		tool_sys_uptime },
	{ "net_status",	"info TCP/IP (ip, gateway, status tcp)",
	  QB_RISK_SAFE,		tool_net_status },
};

unsigned
qb_tool_count(void)
{
	return sizeof qb_tools / sizeof qb_tools[0];
}

const struct qb_tooldef *
qb_tool_at(unsigned i)
{
	if (i >= qb_tool_count())
		return 0;
	return &qb_tools[i];
}

const struct qb_tooldef *
qb_tool_find(const char *name)
{
	unsigned	i, k;
	const char	*tn;

	for (i = 0; i < qb_tool_count(); i++) {
		tn = qb_tools[i].name;
		for (k = 0; tn[k] && name[k] && tn[k] == name[k]; k++)
			;
		if (!tn[k] && !name[k])
			return &qb_tools[i];
	}
	return 0;
}
