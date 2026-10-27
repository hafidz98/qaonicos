/*
 * ufpu.c - K1 userspace leg: user-mode VFP/NEON pattern must survive
 * an IPC park/wake cycle (blocking RECV with a deadline, the
 * IPC-2 path through thread_block -> scheduler -> switch_context).
 *
 * Sets a distinctive VFP pattern, parks ~2 s on a private port
 * nobody sends to (wakes on deadline = UIPC_ETIME), then reads
 * the pattern back bit-exact.  Alone this leg is vacuous (no
 * second VFP thread runs while it parks); combined with the
 * kernel fpu_selftest two-thread proof it covers the user path
 * end to end: user-mode VFP + trap + park + return.
 *
 * Spawned from the shell: run embed:ufpu (production image).
 * Prints one greppable verdict line: UFPU: PASS / UFPU: FAIL ...
 */
#include "ulib/uipc.h"

#define PARK_MS	2000

static struct uipc_msg	msg;
static unsigned int	pat[64];
static unsigned int	scr[64];

static void
putu(unsigned v)
{
	char b[12];
	int i = 0;
	if (v == 0u) {
		u_put("0");
		return;
	}
	while (v != 0u) {
		b[i++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	}
	while (i > 0) {
		char s[2];
		s[0] = b[--i];
		s[1] = '\0';
		u_put(s);
	}
}

static void
fail(const char *what, int rc)
{
	u_put("UFPU: FAIL ");
	u_put(what);
	u_put(" rc=");
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
	u_put("\n");
	sys_exit(1);
}

static void
fpu_set(void)
{
	unsigned int *p = pat;
	__asm__ volatile (
		"vldmia %0!, {d0-d15}\n"
		"vldmia %0!, {d16-d31}\n"
		"vmsr fpscr, %1\n"
		: "+r" (p) : "r" (0x10000000u) : "memory");
}

static int
fpu_check(void)
{
	unsigned int *s = scr;
	unsigned int got_fpscr;
	int i;

	__asm__ volatile (
		"vstmia %0!, {d0-d15}\n"
		"vstmia %0!, {d16-d31}\n"
		"vmrs %1, fpscr\n"
		: "+r" (s), "=r" (got_fpscr) : : "memory");
	for (i = 0; i < 64; i++)
		if (scr[i] != pat[i])
			return i + 1;
	if (got_fpscr != 0x10000000u)
		return 65;
	return 0;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc, bad, i;
	unsigned port, t0, el;

	for (i = 0; i < 64; i++)
		pat[i] = 0xc0000000u + (unsigned int)i * 0x00010001u;

	rc = uipc_port_alloc();
	if (rc < 0)
		fail("alloc", rc);
	port = (unsigned)rc;

	fpu_set();
	u_put("UFPU: parking deadline=2000\n");
	t0 = sys_uptime();
	rc = uipc_recv(port, &msg, UIPC_MSG_MAX, PARK_MS);
	el = sys_uptime() - t0;
	if (rc != UIPC_ETIME)
		fail("wake-not-etime", rc);
	bad = fpu_check();
	if (bad != 0) {
		u_put("UFPU: FAIL pattern word=");
		putu((unsigned)bad);
		u_put(" elapsed=");
		putu(el);
		u_put("\n");
		sys_exit(1);
	}
	u_put("UFPU: PASS elapsed=");
	putu(el);
	u_put("\n");
	sys_exit(0);
}
