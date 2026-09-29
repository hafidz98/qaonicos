/*
 * user/qabot/policy.c -- policy gate: dicek SEBELUM eksekusi, bukan sesudah.
 *
 * Klasifikasi datang dari tool registry; tool tak dikenal = BLOCK.
 * (Pola "doors" ala agent-harness-skeleton.)
 */
#include "qabot.h"

int
qb_policy_check(const char *name, struct qb_toolcall *tc,
    char *reason, unsigned rlen)
{
	const struct qb_tooldef	*t;

	(void)tc;
	t = qb_tool_find(name);
	if (!t) {
		qb_snprintf(reason, rlen, "tool tak dikenal: %s", name);
		return QB_BLOCK;
	}
	switch (t->risk) {
	case QB_RISK_SAFE:
		qb_snprintf(reason, rlen, "risiko aman");
		return QB_ALLOW;
	case QB_RISK_CONFIRM:
		qb_snprintf(reason, rlen, "butuh konfirmasi");
		return QB_CONFIRM;
	default:
		qb_snprintf(reason, rlen, "risiko diblokir: %s", name);
		return QB_BLOCK;
	}
}
