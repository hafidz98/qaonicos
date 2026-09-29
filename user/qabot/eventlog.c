/*
 * user/qabot/eventlog.c -- event log observability + snprintf mini.
 *
 * Mendukung: %s %u %d %c %%. Cukup untuk log harness.
 */
#include "qabot.h"
#include "../ulib/ulib.h"

typedef __builtin_va_list qb_va;

#define qb_va_start(a, l)	__builtin_va_start(a, l)
#define qb_va_arg(a, t)		__builtin_va_arg(a, t)
#define qb_va_end(a)		__builtin_va_end(a)

static unsigned
qb_putu(char *d, unsigned n, unsigned v)
{
	char	tmp[12];
	unsigned	i, len;

	if (v == 0) {
		if (n > 1)
			*d = '0';
		return 1;
	}
	len = 0;
	while (v && len < sizeof tmp) {
		tmp[len++] = '0' + (v % 10);
		v /= 10;
	}
	i = 0;
	while (len && i + 1 < n)
		d[i++] = tmp[--len];
	return i;
}

static unsigned
qb_vsnprintf(char *d, unsigned n, const char *f, qb_va ap)
{
	unsigned	i, neg;
	int		sv;
	unsigned	uv;
	const char	*s;

	if (n == 0)
		return 0;
	i = 0;
	while (*f && i + 1 < n) {
		if (*f != '%') {
			d[i++] = *f++;
			continue;
		}
		f++;
		switch (*f) {
		case 's':
			s = qb_va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			while (*s && i + 1 < n)
				d[i++] = *s++;
			break;
		case 'u':
			uv = qb_va_arg(ap, unsigned);
			i += qb_putu(d + i, n - i, uv);
			break;
		case 'd':
			sv = qb_va_arg(ap, int);
			neg = 0;
			if (sv < 0) {
				neg = 1;
				sv = -sv;
			}
			if (neg && i + 1 < n)
				d[i++] = '-';
			i += qb_putu(d + i, n - i, (unsigned)sv);
			break;
		case 'c':
			d[i++] = (char)qb_va_arg(ap, int);
			break;
		case '%':
			d[i++] = '%';
			break;
		default:
			d[i++] = '%';
			if (i + 1 < n)
				d[i++] = *f;
			break;
		}
		f++;
	}
	d[i] = '\0';
	return i;
}

unsigned
qb_snprintf(char *d, unsigned n, const char *f, ...)
{
	qb_va	ap;
	unsigned	r;

	qb_va_start(ap, f);
	r = qb_vsnprintf(d, n, f, ap);
	qb_va_end(ap);
	return r;
}

void
qb_emit(struct qb_run *r, const char *fmt, ...)
{
	struct qb_event	*e;
	qb_va		ap;

	if (r->nev >= QB_EVENTS_MAX)
		return;
	e = &r->ev[r->nev++];
	e->t_ms = sys_uptime();
	qb_va_start(ap, fmt);
	qb_vsnprintf(e->text, sizeof e->text, fmt, ap);
	qb_va_end(ap);
}
