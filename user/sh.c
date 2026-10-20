/*
 * user/sh.c -- shell QaonicOS (Q8).
 *
 * Prompt "qaon>", line editor via SYS_READ_CONSOLE, built-in:
 * help, ls, cat, gpio, time, uptime, face, qabot (stub; tanpa spawn).
 */
#include "ulib/ulib.h"

#define	SH_LINE_MAX	120u
#define	SH_ARGS_MAX	8u

static int	sh_time_quality(void);

static char	sh_line[SH_LINE_MAX + 1u];
static char	*sh_argv[SH_ARGS_MAX];
static unsigned	sh_argc;

static unsigned
sh_len(const char *s)
{
	unsigned n;

	for (n = 0; s[n]; n++)
		;
	return n;
}

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
	sh_puts("commands:\n");
	sh_puts("  help            list commands\n");
	sh_puts("  ls [path]       list directory (/ -> /sd)\n");
	sh_puts("  cat <path>      show file contents (/motd.txt, /sd/..., /data/..)\n");
	sh_puts("  put /data/<name> <text> write text to a /data file\n");
	sh_puts("  gpio get <pin>  read GPIO pin\n");
	sh_puts("  gpio set <pin> <0/1> write GPIO pin\n");
	sh_puts("  time            wall clock (0=no NTP yet)\n");
	sh_puts("  uptime          ms since boot\n");
	sh_puts("  face <0-7> [text] set face expression\n");
	sh_puts("  qabot           Qabot daemon info\n");
	sh_puts("  qfetch          system summary (neofetch-style)\n");
	sh_puts("  bootctl [crash] boot state / arm crash simulation\n");
	sh_puts("  bootlog         persistent boot history\n");
	sh_puts("  setup [name <x>|tz <x>|done|reset]  first-boot setup\n");
	sh_puts("  update [stage <ver>|sabotage]  A/B update manager\n");
	sh_puts("  run <path>      run program (Q9 spawn)\n");
}

/* --- littlefs (/data) syscall wrappers (B1.5) --- */

static int
sh_lfs_mount(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #94\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : : "r7", "lr", "memory", "cc");
	return ret;
}

static int
sh_lfs_list(char *buf, unsigned max)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #95\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (buf), "r" (max)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

static int
sh_lfs_read(const char *path, char *buf, unsigned max)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #96\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (path), "r" (buf), "r" (max)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

static int
sh_lfs_write(const char *path, const char *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #97\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (path), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

static int
sh_is_data(const char *p)
{
	return p[0] == '/' && p[1] == 'd' && p[2] == 'a' &&
	    p[3] == 't' && p[4] == 'a' &&
	    (p[5] == 0 || p[5] == '/');
}

static void
bi_ls(void)
{
	const char	*pp;
	static char	buf[512];
	int		n;
	unsigned	i;

	/* "/" dipetakan ke /sd (FAT mount). Path /sd/... langsung. */
	pp = sh_argc > 1 ? sh_argv[1] : "/sd";
	if (sh_streq(pp, "/"))
		pp = "/sd";
	if (sh_is_data(pp)) {
		if (sh_lfs_mount() != 0) {
			sh_puts("ls: /data mount failed\n");
			return;
		}
		n = sh_lfs_list(buf, sizeof(buf) - 1u);
		if (n < 0) {
			sh_puts("ls: /data list failed\n");
			return;
		}
		buf[sizeof(buf) - 1u] = 0;
		sh_puts("/data: ");
		sh_puts(buf);
		if (n == 0)
			sh_puts("(empty)");
		sh_puts("\n");
		return;
	}
	n = sys_readdir(pp, buf, sizeof(buf) - 1u);
	if (n < 0) {
		sh_puts("ls: '");
		sh_puts(pp);
		sh_puts("' failed (try /sd)\n");
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
	int		fd, n;

	if (sh_argc < 2) {
		sh_puts("cat: need path (e.g. cat /motd.txt, cat /sd/T/A.BIN)\n");
		return;
	}
	/* littlefs /data/<name>. */
	if (sh_is_data(sh_argv[1]) && sh_argv[1][5] == '/') {
		if (sh_lfs_mount() != 0) {
			sh_puts("cat: /data mount failed\n");
			return;
		}
		n = sh_lfs_read(sh_argv[1] + 6, buf, sizeof(buf) - 1u);
		if (n < 0) {
			sh_puts("cat: '");
			sh_puts(sh_argv[1]);
			sh_puts("' not found in /data\n");
			return;
		}
		buf[n] = 0;
		sh_puts(buf);
		if (n > 0 && buf[n - 1] != '\n')
			sh_puts("\n");
		return;
	}
	/* Coba FS memori dulu (untuk /motd.txt dkk), lalu FAT (/sd/...). */
	fd = sys_open(sh_argv[1], 0u);
	if (fd >= 0) {
		n = sys_read(fd, buf, sizeof(buf) - 1u);
		sys_close(fd);
		if (n < 0) {
			sh_puts("cat: read failed\n");
			return;
		}
		buf[n] = 0;
		sh_puts(buf);
		if (n > 0 && buf[n - 1] != '\n')
			sh_puts("\n");
		return;
	}
	/* Fallback ke FAT. */
	n = sys_fat_read(sh_argv[1], buf, sizeof(buf) - 1u);
	if (n < 0) {
		sh_puts("cat: '");
		sh_puts(sh_argv[1]);
		sh_puts("' not found\n");
		return;
	}
	buf[n] = 0;
	sh_puts(buf);
	if (n > 0 && buf[n - 1] != '\n')
		sh_puts("\n");
}

/* put /data/<name> <text...> -- write text into a littlefs file. */
static void
bi_put(void)
{
	static char	text[256];
	unsigned	i, pos = 0;

	if (sh_argc < 3 || !sh_is_data(sh_argv[1]) ||
	    sh_argv[1][5] != '/') {
		sh_puts("put: usage: put /data/<name> <text>\n");
		return;
	}
	for (i = 2; i < sh_argc; i++) {
		unsigned j;

		if (pos && pos < sizeof(text) - 1u)
			text[pos++] = ' ';
		for (j = 0; sh_argv[i][j] && pos < sizeof(text) - 1u; j++)
			text[pos++] = sh_argv[i][j];
	}
	text[pos] = 0;
	if (sh_lfs_mount() != 0) {
		sh_puts("put: /data mount failed\n");
		return;
	}
	if (sh_lfs_write(sh_argv[1] + 6, text, pos) != 0) {
		sh_puts("put: write failed\n");
		return;
	}
	sh_puts("put: ok (");
	sh_putu(pos);
	sh_puts(" bytes)\n");
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
	if (sh_time_quality() == 1)
		sh_puts(" (NTP synced)");
	else
		sh_puts(" (not NTP synced)");
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

/* --- boot state (B2) wrappers + builtins --- */

static int
sh_bootstate(unsigned sub, unsigned *info)
{
	register unsigned _r0 __asm__("r0") = sub;
	register unsigned _r1 __asm__("r1") = (unsigned)info;
	int ret;
	__asm__ volatile(
		"mov r7, #98\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static int
sh_bootlog(unsigned idx, char *rec)
{
	register unsigned _r0 __asm__("r0") = idx;
	register unsigned _r1 __asm__("r1") = (unsigned)rec;
	int ret;
	__asm__ volatile(
		"mov r7, #99\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static void
bi_bootctl(void)
{
	unsigned info[4];

	if (sh_argc > 1 && sh_streq(sh_argv[1], "crash")) {
		if (sh_bootstate(2u, info) == 0)
			sh_puts("bootctl: crash armed (next boot -> safe mode)\n");
		else
			sh_puts("bootctl: crash arm failed\n");
		return;
	}
	if (sh_bootstate(0u, info) != 0) {
		sh_puts("bootctl: state unavailable\n");
		return;
	}
	sh_puts("boot #");
	sh_putu(info[0]);
	sh_puts("  fail_count=");
	sh_putu(info[1]);
	sh_puts("  safe_mode=");
	sh_putu(info[2]);
	sh_puts("  last_reason=");
	sh_putu(info[3]);
	sh_puts(" (0=unknown 1=power 2=crash 3=watchdog)\n");
}

static int
sh_time_quality(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #100\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : : "r7", "lr", "memory", "cc");
	return ret;
}

/* setup: B3 first-boot provisioning (OS side). */
static void
bi_setup(void)
{
	unsigned info[4];
	static char buf[40];
	int n;

	if (sh_argc > 1 && sh_streq(sh_argv[1], "done")) {
		if (sh_bootstate(4u, info) == 0)
			sh_puts("setup: done -- device provisioned\n");
		else
			sh_puts("setup: failed\n");
		return;
	}
	if (sh_argc > 1 && sh_streq(sh_argv[1], "reset")) {
		if (sh_bootstate(5u, info) == 0)
			sh_puts("setup: provisioning cleared (next boot = first boot)\n");
		else
			sh_puts("setup: failed\n");
		return;
	}
	if (sh_argc > 2 && sh_streq(sh_argv[1], "name")) {
		if (sh_lfs_mount() != 0 ||
		    sh_lfs_write("cfg-name", sh_argv[2],
			sh_len(sh_argv[2])) != 0)
			sh_puts("setup: write failed\n");
		else
			sh_puts("setup: name saved\n");
		return;
	}
	if (sh_argc > 2 && sh_streq(sh_argv[1], "tz")) {
		if (sh_lfs_mount() != 0 ||
		    sh_lfs_write("cfg-tz", sh_argv[2],
			sh_len(sh_argv[2])) != 0)
			sh_puts("setup: write failed\n");
		else
			sh_puts("setup: tz saved\n");
		return;
	}
	/* status */
	n = sh_bootstate(3u, info);
	sh_puts("setup: ");
	sh_puts(n >= 0 && (n & 1) ? "provisioned" : "NOT provisioned");
	sh_puts("\n  name: ");
	if (sh_lfs_mount() == 0 &&
	    (n = sh_lfs_read("cfg-name", buf, sizeof(buf) - 1u)) > 0) {
		buf[n] = 0;
		sh_puts(buf);
	} else {
		sh_puts("(unset)");
	}
	sh_puts("\n  tz: ");
	if (sh_lfs_mount() == 0 &&
	    (n = sh_lfs_read("cfg-tz", buf, sizeof(buf) - 1u)) > 0) {
		buf[n] = 0;
		sh_puts(buf);
	} else {
		sh_puts("(unset)");
	}
	sh_puts("\n");
}

static int
sh_bootstate_arg(unsigned sub, unsigned arg)
{
	register unsigned _r0 __asm__("r0") = sub;
	register unsigned _r1 __asm__("r1") = arg;
	int ret;
	__asm__ volatile(
		"mov r7, #98\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* update: B4 A/B update manager (status / stage / sabotage). */
static void
bi_update(void)
{
	unsigned info[4];

	if (sh_argc > 2 && sh_streq(sh_argv[1], "stage")) {
		unsigned v = sh_atou(sh_argv[2]);

		{
			int r = sh_bootstate_arg(7u, v);

			if (r == 0)
				sh_puts("update: staged (reboot to trial)\n");
			else if (r == -2)
				sh_puts("update: rejected (downgrade)\n");
			else
				sh_puts("update: stage failed (busy?)\n");
		}
		return;
	}
	if (sh_argc > 1 && sh_streq(sh_argv[1], "sabotage")) {
		if (sh_bootstate(8u, info) == 0)
			sh_puts("update: sabotage armed (next boot unconfirmed)\n");
		else
			sh_puts("update: sabotage failed\n");
		return;
	}
	if (sh_bootstate(6u, info) != 0) {
		sh_puts("update: state unavailable\n");
		return;
	}
	sh_puts("update: version=");
	sh_putu(info[0]);
	sh_puts(" slot=");
	sh_puts(info[1] ? "B" : "A");
	sh_puts(" state=");
	sh_puts(info[2] == 0 ? "idle" : info[2] == 1 ? "staged" : "trial");
	if (info[2] != 0) {
		sh_puts(" pending_ver=");
		sh_putu(info[3]);
	}
	sh_puts("\n");
}

static void
bi_bootlog(void)
{
	/* Word array, not char[]: fields are read as words, and a
	 * char buffer is not guaranteed 4-byte aligned (fault 0xdab7
	 * killed the shell when it landed unaligned). */
	static unsigned int rec[16];
	unsigned n, i;
	int cnt = sh_bootlog(0xffffffffu, (char *)rec);

	if (cnt < 0) {
		sh_puts("bootlog: unavailable\n");
		return;
	}
	n = (unsigned)cnt;
	sh_puts("bootlog: ");
	sh_putu(n);
	sh_puts(" records\n");
	for (i = 0; i < n; i++) {
		if (sh_bootlog(i, (char *)rec) != 0)
			continue;
		sh_puts("boot #");
		sh_putu(rec[2]);
		sh_puts(" ev=");
		sh_putu(rec[3]);
		sh_puts(" fail=");
		sh_putu(rec[4]);
		sh_puts(" up=");
		sh_putu(rec[5]);
		sh_puts("ms ");
		((char *)rec)[63] = 0;
		sh_puts((char *)rec + 24);
		sh_puts("\n");
	}
	if (n == 0)
		sh_puts("bootlog: (empty)\n");
}

/* qfetch: neofetch-style system summary, QaonicOS edition. */
static const char *qf_logo[] = {
	"   QQQ   A   OOO  N   N I  CCC  OOO   SSS  ",
	"  Q   Q A A O   O NN  N I C    O   O S     ",
	"  Q   QAAAAAO   O N N N I C    O   O  SSS  ",
	"  Q  QQ A A O   O N  NN I C    O   O     S ",
	"   QQQQ A A  OOO  N   N I  CCC  OOO  SSSS  ",
	"                                           ",
	"                                           ",
	"                                           ",
	"                                           ",
	"                                           ",
	"                                           ",
	"                                           ",
};

static void
bi_qfetch(void)
{
	struct qaon_stat st;
	unsigned up_s;

	if (u_stat(&st) != 0) {
		sh_puts("qfetch: stat unavailable\n");
		return;
	}
	up_s = st.uptime_ms / 1000u;
	sh_puts(qf_logo[0]);
	sh_puts("qaon@qaonicos\n");
	sh_puts(qf_logo[1]);
	sh_puts("-----------------\n");
	sh_puts(qf_logo[2]);
	sh_puts("OS: QaonicOS (Mach 3.0)\n");
	sh_puts(qf_logo[3]);
	sh_puts("SoC: Rockchip RV1103 (target)\n");
	sh_puts(qf_logo[4]);
	sh_puts("CPU: Cortex-A7 @ 1.2 GHz\n");
	sh_puts(qf_logo[5]);
	sh_puts("Uptime: ");
	sh_putu(up_s);
	sh_puts(" s\n");
	sh_puts(qf_logo[6]);
	sh_puts("Memory: ");
	if (st.mem_used_kb != QAON_UNKNOWN) {
		sh_putu(st.mem_used_kb);
		sh_puts(" / ");
		sh_putu(st.mem_total_kb);
		sh_puts(" KB");
	} else {
		sh_puts("unknown");
	}
	sh_puts("\n");
	sh_puts(qf_logo[7]);
	sh_puts("Threads: ");
	sh_putu(st.nthreads);
	sh_puts("\n");
	sh_puts(qf_logo[8]);
	sh_puts("Net: rx ");
	sh_putu(st.net_rx_kb);
	sh_puts(" KB, tx ");
	sh_putu(st.net_tx_kb);
	sh_puts(" KB\n");
	sh_puts(qf_logo[9]);
	sh_puts("Shell: qaon> (Q8/Q9)\n");
	sh_puts(qf_logo[10]);
	sh_puts("Agent: Qabot\n");
	sh_puts(qf_logo[11]);
	sh_puts("Display: QEMU virtio-gpu / SPI ST7789\n");
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
	sys_console_takeover();	/* Q9: sh pemilik eksklusif console */
	sh_puts("\n");
	sh_puts("QaonicOS ready. CLI mode.\n");
	sh_puts("Type 'help' for commands.\n");
	sh_puts("\n");
	/* Q9 self-test: spawn embed:uls, verifikasi exit code. */
	{
		int rc, w;
		sh_puts("[selftest] spawn embed:uls...\n");
		rc = sys_spawn("embed:uls");
		if (rc != 0) {
			sh_puts("[selftest] FAIL: spawn gagal\n");
		} else {
			for (;;) {
				w = sys_spawn_wait();
				if (w != -1)
					break;
				sys_yield();
			}
			sh_puts("[selftest] spawn exit=");
			sh_putu((unsigned)w);
			sh_puts(w == 0 ? " PASS\n" : " FAIL\n");
		}
	}
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
		else if (sh_streq(sh_argv[0], "put"))
			bi_put();
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
		else if (sh_streq(sh_argv[0], "qfetch"))
			bi_qfetch();
		else if (sh_streq(sh_argv[0], "bootctl"))
			bi_bootctl();
		else if (sh_streq(sh_argv[0], "bootlog"))
			bi_bootlog();
		else if (sh_streq(sh_argv[0], "setup"))
			bi_setup();
		else if (sh_streq(sh_argv[0], "update"))
			bi_update();
		else if (sh_streq(sh_argv[0], "run"))
			bi_run();
		else {
			sh_puts("perintah tak dikenal: ");
			sh_puts(sh_argv[0]);
			sh_puts("\n");
		}
	}
}
