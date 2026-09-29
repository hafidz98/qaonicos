/*
 * user/qabot/history.c -- message history append-only (state kanonis).
 */
#include "qabot.h"

void
qb_hist_init(struct qb_history *h)
{
	h->n = 0;
}

int
qb_hist_add(struct qb_history *h, unsigned char role, const char *text)
{
	struct qb_msg	*m;
	unsigned	i;

	if (h->n >= QB_HIST_MAX)
		return -1;
	m = &h->m[h->n++];
	m->role = role;
	for (i = 0; i + 1 < QB_MSG_MAX && text[i]; i++)
		m->text[i] = text[i];
	m->text[i] = '\0';
	return 0;
}
