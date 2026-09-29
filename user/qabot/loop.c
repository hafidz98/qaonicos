/*
 * user/qabot/loop.c -- agent loop ReAct + guardrail.
 *
 * prompt = compose(history) -> chat -> tool_calls -> policy gate ->
 * execute -> observe -> ... -> final. Guard: max steps / max waktu.
 */
#include "qabot.h"
#include "../ulib/ulib.h"

/* Parse "TOOL:nama k=v k=v" -> qb_toolcall. */
int
qb_parse_toolcall(const char *line, struct qb_toolcall *tc)
{
	const char	*p;
	unsigned	i, k;

	for (i = 0; i < 32; i++)
		tc->name[i] = '\0';
	tc->nargs = 0;

	p = line + 5;	/* lewati "TOOL:" */
	for (i = 0; i < 31 && *p && *p != ' ' && *p != '\t'; i++, p++)
		tc->name[i] = *p;
	tc->name[i] = '\0';
	if (!tc->name[0])
		return -1;

	while (*p == ' ' || *p == '\t')
		p++;
	while (*p && tc->nargs < QB_ARGS_MAX) {
		/* kunci */
		for (k = 0; k + 1 < QB_ARGK_MAX && *p && *p != '=' &&
		     *p != ' ' && *p != '\t'; k++, p++)
			tc->akey[tc->nargs][k] = *p;
		tc->akey[tc->nargs][k] = '\0';
		if (*p != '=')
			break;
		p++;
		/* nilai */
		for (k = 0; k + 1 < QB_ARGV_MAX && *p && *p != ' ' &&
		     *p != '\t'; k++, p++)
			tc->aval[tc->nargs][k] = *p;
		tc->aval[tc->nargs][k] = '\0';
		tc->nargs++;
		while (*p == ' ' || *p == '\t')
			p++;
	}
	return 0;
}

int
qb_starts(const char *s, const char *pre)
{
	while (*pre) {
		if (*s != *pre)
			return 0;
		s++;
		pre++;
	}
	return 1;
}

int
qb_ev_contains(struct qb_run *r, const char *sub)
{
	unsigned	e, i, k;
	int		hit;

	for (e = 0; e < r->nev; e++) {
		hit = 0;
		for (i = 0; r->ev[e].text[i] && !hit; i++) {
			for (k = 0; sub[k] && r->ev[e].text[i + k] == sub[k];
			     k++)
				;
			if (!sub[k])
				hit = 1;
		}
		if (hit)
			return 1;
	}
	return 0;
}

int
qb_run_task(struct qb_run *r, const char *prompt, struct qb_provider *p)
{
	struct qb_toolcall	tc;
	const struct qb_tooldef	*td;
	const char		*resp;
	char			out[QB_MSG_MAX];
	char			reason[64];
	char			calltxt[QB_MSG_MAX];
	unsigned		t0, dt;
	int			verdict, rc;

	r->steps = 0;
	r->status = QB_DONE;
	r->final[0] = '\0';
	qb_hist_init(&r->h);
	qb_hist_add(&r->h, QB_USER, prompt);
	qb_emit(r, "task mulai: %s", prompt);

	t0 = sys_uptime();
	for (;;) {
		dt = sys_uptime() - t0;
		if (dt > QB_MAX_MS) {
			r->status = QB_GUARD_TIME;
			qb_emit(r, "guard: waktu habis (%ums)", dt);
			break;
		}
		if (r->steps >= QB_MAX_STEPS) {
			r->status = QB_GUARD_STEPS;
			qb_emit(r, "guard: max steps %u", QB_MAX_STEPS);
			break;
		}
		r->steps++;

		resp = p->chat(p->ctx, &r->h);
		qb_emit(r, "provider: %s", resp);

		if (qb_starts(resp, "FINAL:")) {
			qb_hist_add(&r->h, QB_ASSISTANT, resp + 6);
			qb_snprintf(r->final, sizeof r->final, "%s", resp + 6);
			qb_emit(r, "final: %s", resp + 6);
			break;
		}
		if (!qb_starts(resp, "TOOL:") ||
		    qb_parse_toolcall(resp, &tc) != 0) {
			qb_emit(r, "provider: format tak dikenal, stop");
			break;
		}

		qb_snprintf(calltxt, sizeof calltxt, "call %s (%u arg)",
		    tc.name, tc.nargs);
		qb_hist_add(&r->h, QB_ASSISTANT, calltxt);

		/* POLICY GATE -- sebelum eksekusi. */
		verdict = qb_policy_check(tc.name, &tc, reason, sizeof reason);
		qb_emit(r, "gate: %s -> %s (%s)", tc.name,
		    verdict == QB_ALLOW ? "ALLOW" :
		    verdict == QB_CONFIRM ? "CONFIRM" : "BLOCK", reason);

		if (verdict == QB_BLOCK) {
			qb_snprintf(out, sizeof out, "BLOCKED: %s", reason);
			qb_hist_add(&r->h, QB_TOOL, out);
			continue;
		}
		if (verdict == QB_CONFIRM) {
			if (!r->auto_confirm) {
				qb_snprintf(out, sizeof out,
				    "BLOCKED: konfirmasi ditolak (device)");
				qb_hist_add(&r->h, QB_TOOL, out);
				qb_emit(r, "confirm: ditolak");
				continue;
			}
			qb_emit(r, "confirm: auto-yes (mode uji)");
		}

		td = qb_tool_find(tc.name);
		rc = td->exec(&tc, out, sizeof out);
		qb_emit(r, "exec: %s -> %s", tc.name, out);
		qb_hist_add(&r->h, QB_TOOL, out);
		(void)rc;
	}

	qb_emit(r, "task selesai: status=%d steps=%u", r->status, r->steps);
	return r->status;
}
