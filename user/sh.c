/*
 * user/sh.c -- shell QaonicOS (Q8).
 *
 * Prompt "qaon>", line editor via SYS_READ_CONSOLE, built-in:
 * help, ls, cat, gpio, time, uptime, face, qabot (stub; tanpa spawn).
 */
#include "ulib/ulib.h"
#include "ulib/uipc.h"

#define	SH_LINE_MAX	120u
#define	SH_ARGS_MAX	8u

static int	sh_time_quality(void);

static char	sh_line[SH_LINE_MAX + 1u];
static char	*sh_argv[SH_ARGS_MAX];
static unsigned	sh_argc;

/* ONB-1: web shell capture. While cap_on, everything the shell
 * prints is also copied to cap_buf and posted to the kernel web
 * bridge after the command finishes (see the main loop). */
static char	cap_buf[4096];
static unsigned	cap_len;
static int	cap_on;

static void
cap_puts(const char *s, unsigned n)
{
	unsigned i;
	if (!cap_on)
		return;
	for (i = 0; i < n && cap_len < sizeof(cap_buf) - 1u; i++)
		cap_buf[cap_len++] = s[i];
	cap_buf[cap_len] = 0;
}

static int
sh_webui(unsigned sub, char *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #103\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (sub), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

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
	cap_puts(s, n);
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
	for (j = i - 1; j >= 0; j--) {
		sys_write(1, &buf[j], 1u);
		cap_puts(&buf[j], 1u);
	}
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
			/* ONB-1: a queued web-shell command becomes
			 * the input line when the console is idle. */
			if (n == 0u && !cap_on) {
				int w = sh_webui(0u, sh_line,
				    SH_LINE_MAX);
				if (w > 0) {
					cap_on = 1;
					cap_len = 0;
					sh_line[w] = 0;
					return (unsigned)w;
				}
			}
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
	sh_puts("  setup [name <x>|tz <x>|pin <x>|done|reset]  first-boot setup\n");
	sh_puts("  gui                   start the GUI dashboard\n");
	sh_puts("  key <up|down|left|right|ok|back|menu>  inject a button event (dev/test)\n");
	sh_puts("  ipc resolve|send|req|park  IPC-3 harness (dev/test)\n");
	sh_puts("  update [stage <ver>|stagepkg <path>|cancel|sabotage]  A/B updates\n");
	sh_puts("  reboot          software reset (bootstate reason 4)\n");
	sh_puts("  poweroff        power off (bootstate reason 4)\n");
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
	sh_puts(" (0=unknown 1=power 2=crash 3=watchdog 4=software)\n");
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
	if (sh_argc > 2 && sh_streq(sh_argv[1], "pin")) {
		if (sh_webui(2u, sh_argv[2], sh_len(sh_argv[2])) == 0)
			sh_puts("setup: PIN saved\n");
		else
			sh_puts("setup: PIN invalid (4-16 chars)\n");
		return;
	}
	if (sh_argc > 1 && sh_streq(sh_argv[1], "mode")) {
		n = sh_bootstate(3u, info);
		if (sh_argc > 2 && sh_streq(sh_argv[2], "user")) {
			sh_bootstate(11u, (unsigned *)1u);
			sh_puts("setup: mode=user (GUI autostart, next boot)\n");
		} else if (sh_argc > 2 && sh_streq(sh_argv[2], "dev")) {
			sh_bootstate(11u, (unsigned *)0u);
			sh_puts("setup: mode=dev (shell first, next boot)\n");
		} else {
			sh_puts("setup: mode=");
			sh_puts(n >= 0 && (n & 4) ? "user (GUI autostart)"
			    : "dev (shell first)");
			sh_puts(" -- 'setup mode user|dev'\n");
		}
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
	sh_puts("\n  pin: ");
	sh_puts(sh_webui(3u, (char *)0, 0u) > 0 ? "set" : "(unset)");
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
bi_gui(void)
{
	int rc;
	register unsigned _r7 __asm__("r7") = 102u;
	__asm__ volatile(
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (rc) : "r" (_r7) : "lr", "memory", "cc");
	if (rc == 0)
		sh_puts("gui: starting dashboard (face)...\n");
	else if (rc == 1)
		sh_puts("gui: dashboard already running\n");
	else
		sh_puts("gui: unavailable (safe mode)\n");
}

/* key: inject a button event into the kernel event queue
 * (dev/test -- in QEMU the shell owns the console, so GUI
 * buttons are driven with this; on hardware GPIO feeds the
 * same queue). */
static void
bi_key(void)
{
	static const char *names[] = {
		"up", "down", "left", "right", "ok", "back", "menu",
	};
	int i;

	if (sh_argc < 2) {
		sh_puts("usage: key <up|down|left|right|ok|back|menu>\n");
		return;
	}
	for (i = 0; i < 7; i++) {
		if (sh_streq(sh_argv[1], names[i])) {
			if (sys_event_inject(i) == 0)
				sh_puts("key: injected\n");
			else
				sh_puts("key: inject failed\n");
			return;
		}
	}
	sh_puts("key: unknown button\n");
}


/* ipc: IPC-3 harness builtins (dev/test only). sh is the
/* ipc: IPC-4 harness builtins (dev/test only). sh is the
 * one persistent userspace task, so it holds send and
 * receive names across spawn runs -- a spawn child
 * cannot (it cannot spawn, and its space dies with
 * it). Subcommands:
 *   ipc register <name> alloc a receive port, bind the
 *                       name, hold the port
 *   ipc lookup <name>   look the name up, hold the
 *                       send name, print send + gen
 *   ipc gen <name>      print the name's generation
 *   ipc qlimit <n>      setqlimit on the held port
 *   ipc destroy         destroy the held receive port
 *   ipc send <id>       send an 8-byte message (body
 *                       A5 5A 00..) to the held name
 *   ipc sendbig <id>    send an oversize message
 *                       (4124 B total) to the held
 *                       name; the glue rejects it
 *                       before copying, so the small
 *                       buffer is safe
 *   ipc pollrecv        poll the held receive port
 *   ipc req <id>        alloc a reply port, send a
 *                       reply-carrying request to the
 *                       held name, hold the reply port
 *   ipc park <ms>       blocking recv on the held
 *                       reply port (unsigned ms only:
 *                       a stranded park would hang
 *                       the console owner)
 *   ipc rpc <name> <id> RPC to a registry name (5 s
 *                       deadline) via uipc_rpc_user
 * Each prints one greppable "IPC: ..." line. */
static unsigned	ipc_hold;
static unsigned	ipc_reply;
static unsigned	ipc_recv_hold;
static unsigned	ipc_buf[10];

static void
ipc_puti(int rc)
{
	if (rc < 0) {
		sh_puts("-");
		sh_putu((unsigned)-rc);
	} else
		sh_putu((unsigned)rc);
}

static void
bi_ipc(void)
{
	int rc;

	if (sh_argc < 2) {
		sh_puts("usage: ipc register|lookup|gen|qlimit|destroy|send|sendbig|pollrecv|req|park|rpc ...\n");
		return;
	}
	if (sh_streq(sh_argv[1], "register")) {
		if (sh_argc < 3) {
			sh_puts("usage: ipc register <name>\n");
			return;
		}
		rc = uipc_port_alloc();
		if (rc < 0) {
			sh_puts("IPC: reg ");
			sh_puts(sh_argv[2]);
			sh_puts(" rc=");
			ipc_puti(rc);
			sh_puts("\n");
			return;
		}
		ipc_recv_hold = (unsigned)rc;
		rc = uipc_register(sh_argv[2], ipc_recv_hold);
		sh_puts("IPC: reg ");
		sh_puts(sh_argv[2]);
		sh_puts(" rc=");
		ipc_puti(rc);
		sh_puts(" port=");
		sh_putu(ipc_recv_hold);
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "lookup")) {
		int gen;

		if (sh_argc < 3) {
			sh_puts("usage: ipc lookup <name>\n");
			return;
		}
		rc = uipc_lookup(sh_argv[2]);
		if (rc < 0) {
			sh_puts("IPC: lookup ");
			sh_puts(sh_argv[2]);
			sh_puts(" rc=");
			ipc_puti(rc);
			sh_puts("\n");
			return;
		}
		ipc_hold = (unsigned)rc;
		gen = uipc_reg_gen(sh_argv[2]);
		sh_puts("IPC: lookup ");
		sh_puts(sh_argv[2]);
		sh_puts(" send=");
		sh_putu(ipc_hold);
		sh_puts(" gen=");
		ipc_puti(gen);
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "gen")) {
		if (sh_argc < 3) {
			sh_puts("usage: ipc gen <name>\n");
			return;
		}
		rc = uipc_reg_gen(sh_argv[2]);
		sh_puts("IPC: gen ");
		sh_puts(sh_argv[2]);
		if (rc < 0) {
			sh_puts(" rc=");
			ipc_puti(rc);
		} else {
			sh_puts(" gen=");
			sh_putu((unsigned)rc);
		}
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "qlimit")) {
		if (sh_argc < 3) {
			sh_puts("usage: ipc qlimit <n>\n");
			return;
		}
		rc = uipc_port_setqlimit(ipc_recv_hold,
					 sh_atou(sh_argv[2]));
		sh_puts("IPC: qlimit rc=");
		ipc_puti(rc);
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "destroy")) {
		rc = uipc_port_destroy(ipc_recv_hold);
		sh_puts("IPC: destroy rc=");
		ipc_puti(rc);
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "pollrecv")) {
		rc = uipc_recv_poll(ipc_recv_hold,
				    (struct uipc_msg *)ipc_buf,
				    sizeof(ipc_buf));
		sh_puts("IPC: pollrecv rc=");
		ipc_puti(rc);
		if (rc >= 0) {
			sh_puts(" id=");
			sh_putu(ipc_buf[2]);
		}
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "send") || sh_streq(sh_argv[1], "req") ||
	    sh_streq(sh_argv[1], "sendbig")) {
		unsigned id, reply = 0u, len = UIPC_HDR_SIZE + 8u;

		if (sh_argc < 3) {
			sh_puts("usage: ipc send|sendbig|req <id>\n");
			return;
		}
		id = sh_atou(sh_argv[2]);
		if (sh_streq(sh_argv[1], "sendbig"))
			len = UIPC_HDR_SIZE + 4100u; /* over the cap */
		if (sh_streq(sh_argv[1], "req")) {
			rc = uipc_port_alloc();
			if (rc < 0) {
				sh_puts("IPC: req reply=0 rc=");
				ipc_puti(rc);
				sh_puts("\n");
				return;
			}
			ipc_reply = (unsigned)rc;
			reply = ipc_reply;
		}
		ipc_buf[0] = reply;
		ipc_buf[1] = 0u;
		ipc_buf[2] = id;
		ipc_buf[3] = len;
		ipc_buf[4] = 0u;
		ipc_buf[5] = 0u;
		ipc_buf[6] = 0x5AA5u;
		ipc_buf[7] = 0u;
		rc = uipc_send(ipc_hold, (const struct uipc_msg *)ipc_buf,
			       len);
		if (sh_streq(sh_argv[1], "req")) {
			sh_puts("IPC: req reply=");
			sh_putu(ipc_reply);
			sh_puts(" rc=");
		} else if (sh_streq(sh_argv[1], "sendbig")) {
			sh_puts("IPC: sendbig rc=");
		} else
			sh_puts("IPC: send rc=");
		ipc_puti(rc);
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "park")) {
		unsigned ms;

		if (sh_argc < 3) {
			sh_puts("usage: ipc park <ms>\n");
			return;
		}
		ms = sh_atou(sh_argv[2]);
		rc = uipc_recv(ipc_reply, (struct uipc_msg *)ipc_buf,
			       sizeof(ipc_buf), (int)ms);
		sh_puts("IPC: park rc=");
		ipc_puti(rc);
		if (rc >= 0) {
			/* A delivered message: print its id so
			 * the harness can tell a send-once
			 * notification (id 71) from a reply. */
			sh_puts(" id=");
			sh_putu(ipc_buf[2]);
		}
		sh_puts("\n");
		return;
	}
	if (sh_streq(sh_argv[1], "rpc")) {
		unsigned id;

		if (sh_argc < 4) {
			sh_puts("usage: ipc rpc <name> <id>\n");
			return;
		}
		id = sh_atou(sh_argv[3]);
		ipc_buf[0] = 0u;
		ipc_buf[1] = 0u;
		ipc_buf[2] = id;
		ipc_buf[3] = UIPC_HDR_SIZE + 8u;
		ipc_buf[4] = 0u;
		ipc_buf[5] = 0u;
		ipc_buf[6] = 0x5AA5u;
		ipc_buf[7] = 0u;
		rc = uipc_rpc_user(sh_argv[2],
				   (struct uipc_msg *)ipc_buf,
				   UIPC_HDR_SIZE + 8u, 5000);
		sh_puts("IPC: rpc ");
		sh_puts(sh_argv[2]);
		sh_puts(" rc=");
		ipc_puti(rc);
		if (rc >= 0) {
			sh_puts(" id=");
			sh_putu(ipc_buf[2]);
		}
		sh_puts("\n");
		return;
	}
	sh_puts("usage: ipc register|lookup|gen|qlimit|destroy|send|sendbig|pollrecv|req|park|rpc ...\n");
}

/* reboot / poweroff: FND-1 (SYS_RESET / SYS_POWEROFF). The
 * kernel records bootstate reason 4 (software request)
 * before acting, so the next boot's bootlog tells the
 * truth. On success these never return. */
static void
bi_reboot(void)
{
	sh_puts("reboot: software reset requested\n");
	if (sys_reset() != 0)
		sh_puts("reboot: failed (platform refused)\n");
}

static void
bi_poweroff(void)
{
	sh_puts("poweroff: software poweroff requested\n");
	if (sys_poweroff() != 0)
		sh_puts("poweroff: failed (platform refused)\n");
}

/* svc 98 with a buffer argument (the sub 9/10 shape), cf.
 * uupdsig's sys_bs_buf. */
static int
sh_bootstate_buf(unsigned sub, const void *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = sub;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = len;
	int ret;
	__asm__ volatile(
		"mov r7, #98\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static int
sh_is_sd(const char *p)
{
	return p[0] == '/' && p[1] == 's' && p[2] == 'd' &&
	    (p[3] == 0 || p[3] == '/');
}

#define	SH_PKG_MAX	8192u	/* kernel package cap (syscall 98/9) */

/* Staging buffer: one byte over the cap so an oversized file
 * is detectable -- both file readers return at most the
 * requested count, so asking for cap+1 separates "exactly at
 * the cap" (legal) from "over the cap" (reject in the shell;
 * a truncated package must never be staged). */
static unsigned char	sh_pkgbuf[SH_PKG_MAX + 1u];

/* update stagepkg <path>: FND-5 update delivery path.
 *
 * Reads a signed QUP1 package FILE from /sd (FAT) or /data
 * (littlefs) and hands the bytes to the existing kernel
 * verify+stage path (syscall 98 sub 9), which verifies the
 * RSA-2048 signature BEFORE staging: -3 bad signature or
 * format, -2 downgrade, -1 busy, 0 staged. The shell adds
 * no format knowledge of its own (ARSITEKTUR.md 4.5: reading
 * the file is a userspace act; verify+stage is the kernel
 * invariant) and never stages a file it could not read in
 * full: the whole-file readers + the cap+1 probe make
 * missing/empty/oversized files shell-side errors. */
static void
bi_update_stagepkg(void)
{
	const char	*path;
	unsigned	info[4];
	int		n, r;

	if (sh_argc < 3) {
		sh_puts("update: usage: update stagepkg "
		    "</sd/FILE.BIN|/data/name>\n");
		return;
	}
	path = sh_argv[2];
	if (sh_is_data(path) && path[5] == '/') {
		if (sh_lfs_mount() != 0) {
			sh_puts("update: stagepkg: /data mount failed\n");
			return;
		}
		n = sh_lfs_read(path + 6, (char *)sh_pkgbuf,
		    sizeof(sh_pkgbuf));
	} else if (sh_is_sd(path) && path[3] == '/') {
		n = sys_fat_read(path, sh_pkgbuf, sizeof(sh_pkgbuf));
	} else {
		sh_puts("update: usage: update stagepkg "
		    "</sd/FILE.BIN|/data/name>\n");
		return;
	}
	if (n < 0) {
		sh_puts("update: stagepkg: cannot read '");
		sh_puts(path);
		sh_puts("'\n");
		return;
	}
	if (n == 0) {
		sh_puts("update: stagepkg: '");
		sh_puts(path);
		sh_puts("' is empty\n");
		return;
	}
	if ((unsigned)n > SH_PKG_MAX) {
		sh_puts("update: stagepkg: '");
		sh_puts(path);
		sh_puts("' too large (> 8192 bytes)\n");
		return;
	}
	r = sh_bootstate_buf(9u, sh_pkgbuf, (unsigned)n);
	if (r == -3) {
		sh_puts("update: rejected (bad signature or format)\n");
		return;
	}
	if (r == -2) {
		sh_puts("update: rejected (downgrade)\n");
		return;
	}
	if (r != 0) {
		sh_puts("update: stagepkg failed (busy)\n");
		return;
	}
	sh_puts("update: stagepkg: ");
	sh_putu((unsigned)n);
	sh_puts(" bytes, version ");
	if (sh_bootstate(6u, info) == 0 && info[2] == 1u)
		sh_putu(info[3]);
	else
		sh_puts("?");
	sh_puts("\n");
	sh_puts("update: staged (reboot to trial)\n");
}

static void
bi_update(void)
{
	unsigned info[4];

	if (sh_argc > 1 && sh_streq(sh_argv[1], "stagepkg")) {
		bi_update_stagepkg();
		return;
	}
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
	if (sh_argc > 1 && sh_streq(sh_argv[1], "cancel")) {
		if (sh_bootstate(10u, info) == 0)
			sh_puts("update: staged update cancelled\n");
		else
			sh_puts("update: nothing staged\n");
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
/* Logo: uniform 4x5 block glyphs, each letter drawn with
 * itself, one space between letters (39 chars + 2 pad = the
 * info column at 41). The previous hand-typed art mixed
 * 3/4/5-wide letters with uneven rows -- row 1 especially
 * read as noise (owner feedback 2026-10-22). */
static const char *qf_logo[] = {
	".QQ. .AA. .OO. N..N IIII .CCC .OO. .SSS  ",
	"Q..Q A..A O..O NN.N .II. C    O..O S     ",
	"Q..Q AAAA O..O N.NN .II. C    O..O .SS.  ",
	"Q.Q. A..A O..O N..N .II. C    O..O    S  ",
	".QQQ A..A .OO. N..N IIII .CCC .OO. SSS.  ",
	"                                         ",
	"                                         ",
	"                                         ",
	"                                         ",
	"                                         ",
	"                                         ",
	"                                         ",
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
		/* ONB-1: post the captured output of a web-shell
		 * command before the next prompt is printed. */
		if (cap_on) {
			(void)sh_webui(1u, cap_buf, cap_len);
			cap_on = 0;
			cap_len = 0;
		}
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
		else if (sh_streq(sh_argv[0], "gui"))
			bi_gui();
		else if (sh_streq(sh_argv[0], "key"))
			bi_key();
		else if (sh_streq(sh_argv[0], "ipc"))
			bi_ipc();
		else if (sh_streq(sh_argv[0], "update"))
			bi_update();
		else if (sh_streq(sh_argv[0], "reboot"))
			bi_reboot();
		else if (sh_streq(sh_argv[0], "poweroff"))
			bi_poweroff();
		else if (sh_streq(sh_argv[0], "run"))
			bi_run();
		else {
			sh_puts("perintah tak dikenal: ");
			sh_puts(sh_argv[0]);
			sh_puts("\n");
		}
	}
}
