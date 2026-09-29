/*
 * user/qabot/json.c -- JSON minimal untuk provider real (Q2c).
 *
 * Builder request chat/completions (history + skema tool) dan parser
 * respons minimal (choices[0].message content / tool_calls -> qb_toolcall).
 * Tanpa lib eksternal; buffer disediakan pemanggil (statis/.bss).
 */
#include "qabot.h"

/* Escape satu string ke dst. Return panjang hasil, -1 bila dst penuh. */
static int
qb_json_escape(const char *src, char *dst, unsigned dstlen)
{
	unsigned di = 0u;

	while (*src) {
		char c = *src++;
		const char *esc = 0;

		if (c == '"')
			esc = "\\\"";
		else if (c == '\\')
			esc = "\\\\";
		else if (c == '\n')
			esc = "\\n";
		else if (c == '\r')
			esc = "\\r";
		else if (c == '\t')
			esc = "\\t";
		if (esc) {
			if (di + 2u >= dstlen)
				return -1;
			dst[di++] = esc[0];
			dst[di++] = esc[1];
		} else {
			if (di + 1u >= dstlen)
				return -1;
			dst[di++] = c;
		}
	}
	if (di >= dstlen)
		return -1;
	dst[di] = 0;
	return (int)di;
}

/*
 * Bangun body JSON chat/completions dari history + daftar tool.
 * Return panjang body, -1 bila out kepenuhan.
 */
int
qb_build_request(struct qb_history *h, char *out, unsigned outlen)
{
	unsigned o = 0u, i, t;
	int r;
	static const char *roles[] = { "user", "assistant", "tool" };

	r = qb_snprintf(out + o, outlen - o,
	    "{\"model\":\"qabot-mock\",\"messages\":[");
	if (r < 0)
		return -1;
	o += (unsigned)r;
	for (i = 0; i < h->n; i++) {
		char esc[QB_MSG_MAX * 2];

		if (qb_json_escape(h->m[i].text, esc, sizeof(esc)) < 0)
			return -1;
		r = qb_snprintf(out + o, outlen - o,
		    "%s{\"role\":\"%s\",\"content\":\"%s\"}",
		    i ? "," : "", roles[h->m[i].role & 3u], esc);
		if (r < 0)
			return -1;
		o += (unsigned)r;
	}
	r = qb_snprintf(out + o, outlen - o, "],\"tools\":[");
	if (r < 0)
		return -1;
	o += (unsigned)r;
	for (t = 0; t < qb_tool_count(); t++) {
		const struct qb_tooldef *td = qb_tool_at(t);
		char ed[96];

		if (qb_json_escape(td->desc, ed, sizeof(ed)) < 0)
			return -1;
		r = qb_snprintf(out + o, outlen - o,
		    "%s{\"type\":\"function\",\"function\":"
		    "{\"name\":\"%s\",\"description\":\"%s\","
		    "\"parameters\":{\"type\":\"object\"}}}",
		    t ? "," : "", td->name, ed);
		if (r < 0)
			return -1;
		o += (unsigned)r;
	}
	r = qb_snprintf(out + o, outlen - o, "]}");
	if (r < 0)
		return -1;
	o += (unsigned)r;
	return (int)o;
}

/* Cari '"key"' lalu ':' lalu string ber-quote; unescape minimal ke out. */
static const char *
json_get_string(const char *p, const char *key, char *out, unsigned outlen)
{
	unsigned ki = 0u, oi = 0u;

	while (*p) {
		/* cocokkan "key" */
		if (*p == '"') {
			const char *q = p + 1;
			ki = 0u;
			while (key[ki] && q[ki] == key[ki])
				ki++;
			if (!key[ki] && q[ki] == '"') {
				p = q + ki + 1;
				while (*p == ' ' || *p == '\t')
					p++;
				if (*p != ':')
					continue;
				p++;
				while (*p == ' ' || *p == '\t')
					p++;
				if (*p != '"')
					return 0;
				p++;
				while (*p && *p != '"') {
					char c = *p++;
					if (c == '\\' && *p) {
						char e = *p++;
						if (e == 'n')
							c = '\n';
						else if (e == 'r')
							c = '\r';
						else if (e == 't')
							c = '\t';
						else
							c = e; /* \" \\ \/ */
					}
					if (oi + 1u >= outlen)
						return 0;
					out[oi++] = c;
				}
				if (*p != '"')
					return 0;
				out[oi] = 0;
				return p + 1;
			}
		}
		p++;
	}
	return 0;
}

/*
 * Parse pasangan flat "k":"v" (atau "k":angka) dari string JSON kecil.
 * Mengisi qb_toolcall (nama sudah diisi pemanggil).
 */
static void
json_parse_args(const char *s, struct qb_toolcall *tc)
{
	tc->nargs = 0;
	while (*s && tc->nargs < QB_ARGS_MAX) {
		const char *k, *v;
		unsigned kl, vl;

		while (*s && *s != '"')
			s++;
		if (!*s)
			break;
		k = ++s;
		while (*s && *s != '"')
			s++;
		kl = (unsigned)(s - k);
		if (!*s)
			break;
		s++;		/* lewati '"' */
		while (*s && *s != ':')
			s++;
		if (!*s)
			break;
		s++;		/* lewati ':' */
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '"') {
			v = ++s;
			while (*s && *s != '"') {
				if (*s == '\\' && s[1])
					s += 2;
				else
					s++;
			}
			vl = (unsigned)(s - v);
			if (*s)
				s++;
		} else {
			v = s;
			while (*s && *s != ',' && *s != '}')
				s++;
			vl = (unsigned)(s - v);
		}
		if (kl && kl < QB_ARGK_MAX && vl < QB_ARGV_MAX) {
			unsigned i;
			for (i = 0; i < kl; i++)
				tc->akey[tc->nargs][i] = k[i];
			tc->akey[tc->nargs][kl] = 0;
			/* unescape ringan untuk nilai */
			{
				unsigned vi = 0u, oi = 0u;
				while (vi < vl && oi + 1u < QB_ARGV_MAX) {
					char c = v[vi++];
					if (c == '\\' && vi < vl)
						c = v[vi++];
					tc->aval[tc->nargs][oi++] = c;
				}
				tc->aval[tc->nargs][oi] = 0;
			}
			tc->nargs++;
		}
	}
}

/*
 * Parse body respons chat/completions.
 * Return 1: toolcall terisi; 0: final_out terisi; -1: gagal.
 */
int
qb_parse_response(const char *body, struct qb_toolcall *tc,
		  char *final_out, unsigned flen)
{
	const char *ptc;
	char args[256];

	ptc = 0;
	{
		const char *p = body;
		while (*p) {
			if (p[0] == '"' && p[1] == 't' &&
			    qb_starts(p + 1, "tool_calls\"")) {
				ptc = p;
				break;
			}
			p++;
		}
	}
	if (ptc) {
		if (!json_get_string(ptc, "name", tc->name,
		    sizeof(tc->name)))
			return -1;
		if (!json_get_string(ptc, "arguments", args, sizeof(args)))
			args[0] = 0;
		json_parse_args(args, tc);
		return 1;
	}
	if (!json_get_string(body, "content", final_out, flen))
		return -1;
	return 0;
}
