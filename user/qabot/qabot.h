/*
 * user/qabot/qabot.h -- Qabot harness v1: tipe inti (Q1).
 *
 * Bare-metal C: semua buffer statis, tanpa malloc/libc.
 * Prinsip: core = logika murni, I/O hanya via syscall.
 */
#ifndef QABOT_H
#define QABOT_H

/* Batas statis. */
#define QB_HIST_MAX	24	/* pesan dalam history */
#define QB_MSG_MAX	256	/* byte per pesan */
#define QB_TOOLS_MAX	8
#define QB_ARGS_MAX	4
#define QB_ARGK_MAX	16
#define QB_ARGV_MAX	48
#define QB_EVENTS_MAX	48
#define QB_EVTXT_MAX	128

/* Peran pesan. */
#define QB_USER		0u
#define QB_ASSISTANT	1u
#define QB_TOOL		2u

struct qb_msg {
	unsigned char	role;
	char		text[QB_MSG_MAX];
};

struct qb_history {
	struct qb_msg	m[QB_HIST_MAX];
	unsigned	n;
};

/* Tool call ter-parse dari "TOOL:nama k=v ...". */
struct qb_toolcall {
	char	name[32];
	char	akey[QB_ARGS_MAX][QB_ARGK_MAX];
	char	aval[QB_ARGS_MAX][QB_ARGV_MAX];
	unsigned	nargs;
};

/* Klasifikasi risiko tool. */
#define QB_RISK_SAFE	0u
#define QB_RISK_CONFIRM	1u
#define QB_RISK_BLOCK	2u

/* Verdict policy gate. */
#define QB_ALLOW	0
#define QB_CONFIRM	1
#define QB_BLOCK	2

struct qb_tooldef {
	const char	*name;
	const char	*desc;
	unsigned char	risk;
	int		(*exec)(struct qb_toolcall *tc,
				char *out, unsigned outlen);
};

/* Status tugas. */
#define QB_DONE		0
#define QB_GUARD_STEPS	1
#define QB_GUARD_TIME	2

/* Guardrail. */
#define QB_MAX_STEPS	8u
#define QB_MAX_MS	30000u

struct qb_event {
	unsigned	t_ms;
	char		text[QB_EVTXT_MAX];
};

struct qb_run {
	struct qb_history	h;
	struct qb_event		ev[QB_EVENTS_MAX];
	unsigned		nev;
	unsigned		steps;
	int			auto_confirm;	/* Q1: 1 = mode uji */
	int			status;
	char			final[QB_MSG_MAX];
};

/* Provider: abstraksi chat. Q1 = mock scripted; Q2 = HTTPS+JSON. */
struct qb_provider {
	const char	*(*chat)(void *ctx, struct qb_history *h);
	void		*ctx;
};

/* history.c */
void	qb_hist_init(struct qb_history *h);
int	qb_hist_add(struct qb_history *h, unsigned char role,
		      const char *text);

/* tools.c */
const struct qb_tooldef	*qb_tool_find(const char *name);
unsigned		 qb_tool_count(void);
const struct qb_tooldef	*qb_tool_at(unsigned i);

/* policy.c */
int	qb_policy_check(const char *name, struct qb_toolcall *tc,
			  char *reason, unsigned rlen);

/* provider.c (mock). */
struct qb_mock_ctx {
	const char	**script;
	unsigned	n;
	unsigned	i;
};
void	qb_mock_init(struct qb_mock_ctx *m, const char **script,
		       unsigned n);
extern struct qb_provider	qb_mock_provider;	/* chat = qb_mock_chat */

/* loop.c */
int	qb_parse_toolcall(const char *line, struct qb_toolcall *tc);
int	qb_run_task(struct qb_run *r, const char *prompt,
		      struct qb_provider *p);
int	qb_ev_contains(struct qb_run *r, const char *sub);

/* eventlog.c */
void	qb_emit(struct qb_run *r, const char *fmt, ...);
unsigned qb_snprintf(char *d, unsigned n, const char *f, ...);

#endif /* QABOT_H */
