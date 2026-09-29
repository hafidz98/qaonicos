/*
 * user/sh.c -- shell QaonicOS (Q8).
 *
 * Prompt "qaon>", line editor via SYS_READ_CONSOLE, built-in:
 * help, ls, cat, gpio, time, uptime, face, qabot (stub; tanpa spawn).
 */
#include "ulib/ulib.h"

#define	SH_LINE_MAX	120u
#define	SH_ARGS_MAX	8u

static char	sh_line[SH_LINE_MAX + 1u];
static char	*sh_argv[SH_ARGS_MAX];
static unsigned	sh_argc;

static void
sh_puts(const char *s)
{
	unsigned	n;

	for (n = 0; s[n]; n++)
		;
	sys_write(1, s, n);
}

static void
sh_putu(unsigned v)
{
	char		buf[12];
	int		i = 0;
	unsigned	t;
	int		j;

	if (v == 0) {
		sh_puts("0");
		return;
	}
	t = v;
	while (t > 0 && i < 11) {
		buf[i++] = (char)('0' + t % 10u);
		t /= 10u;
	}
	for (j = i - 1; j >= 0; j--)
		sys_write(1, &buf[j], 1u);
}

/* Line editor: echo, backspace, Enter. */
static unsigned
sh_readline(void)
{
	unsigned	n = 0u;
	int		c;
	char		ec;

	for (;;) {
		c = sys_read_console();
		if (c < 0) {
			sys_yield();
			continue;
		}
		if (c == '\r' || c == '\n') {
			ec = '\n';
			sys_write(1, &ec, 1u);
			break;
		}
		if (c == 0x7fu || c == 0x08) {
			if (n > 0) {
				n--;
				sh_puts("\b \b");
			}
			continue;
		}
		if (c < 0x20 || c > 0x7e)
			continue;
		if (n < SH_LINE_MAX) {
			sh_line[n++] = (char)c;
			ec = (char)c;
			sys_write(1, &ec, 1u);
		}
	}
	sh_line[n] = 0;
	return n;
}

/* Parser: pecah baris jadi argv (spasi sebagai pemisah). */
static void
sh_parse(void)
{
	unsigned	i = 0u;

	sh_argc = 0u;
	while (sh_line[i] && sh_argc < SH_ARGS_MAX) {
		while (sh_line[i] == ' ' || sh_line[i] == '\t')
			i++;
		if (!sh_line[i])
			break;
		sh_argv[sh_argc++] = &sh_line[i];
		while (sh_line[i] && sh_line[i] != ' ' &&
		    sh_line[i] != '\t')
			i++;
		if (sh_line[i])
			sh_line[i++] = 0;
	}
}

static int
sh_streq(const char *a, const char *b)
{
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static unsigned
sh_atou(const char *s)
{
	unsigned	v = 0u;

	while (*s >= '0' && *s <= '9') {
		v = v * 10u + (unsigned)(*s - '0');
		s++;
	}
	return v;
}

/* --- built-in --- */

static void
bi_help(void)
{
	sh_puts("perintah:\n");
	sh_puts("  help            daftar perintah\n");
	sh_puts("  ls [path]       daftar isi direktori\n");
	sh_puts("  cat <path>      tampilkan isi file\n");
	sh_puts("  gpio get <pin>  baca pin GPIO\n");
	sh_puts("  gpio set <pin> <0/1> tulis pin GPIO\n");
	sh_puts("  time            jam dinding (0=belum NTP)\n");
	sh_puts("  uptime          ms sejak boot\n");
	sh_puts("  face <0-7> [teks] set ekspresi wajah\n");
	sh_puts("  qabot           info daemon Qabot\n");
	sh_puts("  run <path>      jalankan program (Q9 spawn)\n");
}

static void
bi_ls(void)
{
	const char	*pp;
	static char	buf[512];
	int		n;
	unsigned	i;

	pp = sh_argc > 1 ? sh_argv[1] : "/";
	n = sys_readdir(pp, buf, sizeof(buf) - 1u);
	if (n < 0) {
		sh_puts("ls: gagal\n");
		return;
	}
	buf[sizeof(buf) - 1u] = 0;
	sh_puts(pp);
	sh_puts(": ");
	for (i = 0; buf[i] && i < sizeof(buf) - 1u; i++) {
		sys_write(1, &buf[i], 1u);
		if (buf[i] == 0)
			break;
	}
	sh_puts("\n");
	(void)n;
}

static void
bi_cat(void)
{
	static char	buf[512];
	int		n;

	if (sh_argc < 2) {
		sh_puts("cat: butuh path\n");
		return;
	}
	n = sys_fat_read(sh_argv[1], buf, sizeof(buf) - 1u);
	if (n < 0) {
		sh_puts("cat: baca gagal\n");
		return;
	}
	buf[n] = 0;
	sh_puts(buf);
	sh_puts("\n");
}

static void
bi_gpio(void)
{
	unsigned	pin, val;
	int		v;

	if (sh_argc < 3) {
		sh_puts("gpio: get <pin> | set <pin> <0/1>\n");
		return;
	}
	pin = sh_atou(sh_argv[2]);
	if (sh_streq(sh_argv[1], "get")) {
		v = sys_gpio_get(pin);
		if (v < 0)
			sh_puts("gpio: get gagal\n");
		else {
			sh_puts("pin ");
			sh_putu(pin);
			sh_puts(" = ");
			sh_putu((unsigned)v);
			sh_puts("\n");
		}
	} else if (sh_streq(sh_argv[1], "set")) {
		if (sh_argc < 4) {
			sh_puts("gpio set: butuh val\n");
			return;
		}
		val = sh_atou(sh_argv[3]) ? 1u : 0u;
		if (sys_gpio_set(pin, val) != 0)
			sh_puts("gpio: set gagal\n");
		else
			sh_puts("ok\n");
	} else {
		sh_puts("gpio: get|set\n");
	}
}

static void
bi_time(void)
{
	unsigned	t;

	t = sys_time_get();
	sh_puts("time=");
	sh_putu(t);
	if (t == 0)
		sh_puts(" (belum sinkron NTP)");
	sh_puts("\n");
}

static void
bi_uptime(void)
{
	unsigned	ms;

	ms = sys_uptime();
	sh_puts("uptime_ms=");
	sh_putu(ms);
	sh_puts(" (");
	sh_putu(ms / 1000u);
	sh_puts(" dtk)\n");
}

static void
bi_face(void)
{
	unsigned	expr;
	const char	*txt;

	if (sh_argc < 2) {
		sh_puts("face: butuh expr 0-7\n");
		return;
	}
	expr = sh_atou(sh_argv[1]);
	if (expr > 7u) {
		sh_puts("face: expr 0-7\n");
		return;
	}
	txt = sh_argc > 2 ? sh_argv[2] : 0;
	if (sys_face_expr(expr, txt) != 0)
		sh_puts("face: gagal\n");
	else
		sh_puts("ok\n");
}

static void
bi_qabot(void)
{
	sh_puts("qabotd berjalan sebagai daemon terpisah (prompt qabot>).\n");
	sh_puts("Shell ini (qaon>) untuk perintah sistem langsung.\n");
}

/* Q9: run <path> — spawn program, tunggu sampai exit. */
static void
bi_run(void)
{
	int rc, w;
	if (sh_argc < 2) {
		sh_puts("pakai: run <path>\n");
		return;
	}
	rc = sys_spawn(sh_argv[1]);
	if (rc != 0) {
		sh_puts("spawn gagal\n");
		return;
	}
	/* Tunggu child selesai. */
	for (;;) {
		w = sys_spawn_wait();
		if (w != -1)
			break;
		sys_yield();
	}
	sh_puts("exit ");
	sh_putu((unsigned)w);
	sh_puts("\n");
}

void
_start(void)
{
	sh_puts("QaonicOS shell (Q8). ketik help.\n");
	for (;;) {
		sh_puts("qaon> ");
		if (sh_readline() == 0u)
			continue;
		sh_parse();
		if (sh_argc == 0u)
			continue;
		if (sh_streq(sh_argv[0], "help"))
			bi_help();
		else if (sh_streq(sh_argv[0], "ls"))
			bi_ls();
		else if (sh_streq(sh_argv[0], "cat"))
			bi_cat();
		else if (sh_streq(sh_argv[0], "gpio"))
			bi_gpio();
		else if (sh_streq(sh_argv[0], "time"))
			bi_time();
		else if (sh_streq(sh_argv[0], "uptime"))
			bi_uptime();
		else if (sh_streq(sh_argv[0], "face"))
			bi_face();
		else if (sh_streq(sh_argv[0], "qabot"))
			bi_qabot();
		else if (sh_streq(sh_argv[0], "run"))
			bi_run();
		else {
			sh_puts("perintah tak dikenal: ");
			sh_puts(sh_argv[0]);
			sh_puts("\n");
		}
	}
}
