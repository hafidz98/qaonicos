/*
 * uipcld.c - IPC-4 concurrent client daemon
 * (daemon_images entry, NOT a spawn child -- it must
 * run alongside the embed-hook tools server, which is
 * the first pair of live IPC speakers in this track).
 *
 * Marker-gated: at startup it mounts /data and reads
 * "iptoolsc-test"; unless the content is exactly "1"
 * it prints one dormant line and exits cleanly, so
 * every other boot is unaffected (the daemon still
 * launches on `gui` -- one task setup + exit + one
 * boot line -- because daemon_images is static).
 *
 * Active flow (driven by scripts/verify-ipc4.py):
 *   lookup "tools" (gen 1) -> 100 echo RPCs byte-exact
 *   -> structured dispatch RPC -> double-reply RPC
 *   -> DIE RPC (server exits without replying;
 *   expect UIPC_EPEER) -> re-lookup (EDEST) + gen 2
 *   (death bump) -> lookup incarnation 2 (gen 3)
 *   -> 10 echo RPCs -> STOP -> DONE.
 * Every RPC goes through uipc_rpc_user with a 30 s
 * deadline; any validation failure prints FAIL and
 * exits 1. Lookup retries yield between attempts so
 * the server slot gets CPU time.
 *
 * NOT in uprogs[]: test-only daemon, launched by
 * `gui` alongside face/uiapp/qabotd.
 */
#include "ulib/uipc.h"

#define RPC_TIMEOUT_MS	30000

static struct uipc_msg	msg;
static char		marker[8];

static int
sys_lfs_simple(unsigned num)
{
	int ret;
	__asm__ volatile(
		"mov r7, %1\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (num)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static int
sys_lfs_read(const char *path, char *buf, unsigned max)
{
	register unsigned _r0 __asm__("r0") = (unsigned)path;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = max;
	int ret;
	__asm__ volatile(
		"mov r7, #96\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

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
puti(int rc)
{
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
}

static void
fail(const char *what, int rc)
{
	u_put("UIPCLD: FAIL ");
	u_put(what);
	u_put(" rc=");
	puti(rc);
	u_put("\n");
	sys_exit(1);
}

/* One echo RPC (id 0x3000+s) with the IPC-1 payload
 * pattern; validates the XOR reply byte-exact. */
static void
echo_one(unsigned s)
{
	unsigned blen = 8u + ((s * 37u) % 62u) * 4u;
	unsigned j, rc;

	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = (int)(0x3000u + s);
	msg.size = UIPC_HDR_SIZE + blen;
	for (j = 0; j < blen; j++)
		msg.body[j] = (unsigned char)((s * 31u + j) & 0xFFu);
	rc = (unsigned)uipc_rpc_user("tools", &msg,
				      UIPC_HDR_SIZE + blen,
				      RPC_TIMEOUT_MS);
	if ((int)rc != (int)(UIPC_HDR_SIZE + blen))
		fail("echo len", (int)rc);
	if (msg.id != (int)(0x3000u + s))
		fail("echo id", msg.id);
	for (j = 0; j < blen; j++) {
		unsigned char want =
		    (unsigned char)(((s * 31u + j) & 0xFFu) ^ 0xA5u);
		if (msg.body[j] != want)
			fail("echo body", (int)j);
	}
}

/* Lookup "tools" with retries; returns the send name
 * (or exits 1 on timeout). The retry is WALL-CLOCK
 * based (60 s via sys_uptime): a bare yield count
 * burns out in seconds when the scheduler round is
 * fast, and the server may not exist yet when the
 * client starts (it spawns after `gui` via the web
 * shell). The generation is printed by the caller. */
static int
lookup_retry(void)
{
	unsigned t0 = sys_uptime();
	int rc;

	for (;;) {
		rc = uipc_lookup("tools");
		if (rc >= 0)
			return rc;
		if (rc != UIPC_ENORV && rc != UIPC_EDEST)
			fail("lookup", rc);
		if (sys_uptime() - t0 >= 60000u)
			fail("lookup timeout", -9);
		(void)sys_yield();
	}
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc, gen, i;

	/* Marker gate. */
	if (sys_lfs_simple(94u) != 0) {
		u_put("UIPCLD: dormant\n");
		sys_exit(0);
	}
	if (sys_lfs_read("iptoolsc-test", marker,
			 sizeof(marker)) != 1 ||
	    marker[0] != '1') {
		u_put("UIPCLD: dormant\n");
		sys_exit(0);
	}

	/* Phase 1: lookup + 100 echo RPCs. */
	rc = lookup_retry();
	(void)rc;	/* the send name is not used directly;
			 * RPCs go through the registry */
	gen = uipc_reg_gen("tools");
	u_put("UIPCLD: lookup tools gen=");
	puti(gen);
	u_put("\n");
	if (gen != 1)
		fail("lookup gen", gen);
	for (i = 0; i < 100; i++) {
		echo_one((unsigned)i);
		if (i == 24 || i == 49 || i == 74 || i == 99) {
			u_put("UIPCLD: echo ");
			putu((unsigned)(i + 1));
			u_put("/100\n");
		}
	}
	u_put("UIPCLD: PASS echo=100/100\n");

	/* Phase 2: structured dispatch. The reply is a
	 * 20-byte structured result, so the request is
	 * padded to the same 20-byte body (44 total):
	 * the RPC length is both the send length and
	 * the reply budget, and a larger reply than
	 * the budget returns E2BIG with the message
	 * left queued. The server reads only the
	 * first two words. */
	{
		unsigned *w = (unsigned *)msg.body;

		msg.reply = 0u;
		msg.dest = 0u;
		msg.id = 0x3100;
		msg.size = UIPC_HDR_SIZE + 20u;
		w[0] = 0xD15u;
		w[1] = 42u;
		w[2] = 0u;
		w[3] = 0u;
		w[4] = 0u;
		rc = uipc_rpc_user("tools", &msg,
				   UIPC_HDR_SIZE + 20u, RPC_TIMEOUT_MS);
		if (rc != (int)(UIPC_HDR_SIZE + 20u))
			fail("dispatch len", rc);
		if (msg.id != 0x3100)
			fail("dispatch id", msg.id);
		if (w[0] != 0u || w[1] != 3u || w[2] != 43u ||
		    w[3] != 44u || w[4] != 45u)
			fail("dispatch body", (int)w[0]);
	}
	u_put("UIPCLD: dispatch OK\n");

	/* Phase 3: double-reply (first reply arrives). */
	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = 0x3200;
	msg.size = UIPC_HDR_SIZE + 8u;
	msg.body[0] = 'D';
	rc = uipc_rpc_user("tools", &msg,
			   UIPC_HDR_SIZE + 8u, RPC_TIMEOUT_MS);
	if (rc != (int)(UIPC_HDR_SIZE + 8u))
		fail("double len", rc);
	if (msg.id != 0x3200 || msg.body[0] != 'O' ||
	    msg.body[1] != 'K')
		fail("double body", msg.id);
	u_put("UIPCLD: double first id=");
	putu(0x3200u);
	u_put("\n");

	/* Phase 4: DIE -- server exits without replying. */
	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = 0x3300;
	msg.size = UIPC_HDR_SIZE + 8u;
	rc = uipc_rpc_user("tools", &msg,
			   UIPC_HDR_SIZE + 8u, RPC_TIMEOUT_MS);
	if (rc != UIPC_EPEER)
		fail("die rc", rc);
	u_put("UIPCLD: die rc=-12\n");

	/* Phase 5: re-lookup must fail dead; gen bumped. */
	rc = uipc_lookup("tools");
	u_put("UIPCLD: regen lookup rc=");
	puti(rc);
	u_put("\n");
	if (rc != UIPC_EDEST)
		fail("regen lookup", rc);
	gen = uipc_reg_gen("tools");
	u_put("UIPCLD: regen gen=");
	puti(gen);
	u_put("\n");
	if (gen != 2)
		fail("regen gen", gen);

	/* Phase 6: incarnation 2 (gen 3) + 10 echo RPCs. */
	rc = lookup_retry();
	(void)rc;
	gen = uipc_reg_gen("tools");
	u_put("UIPCLD: lookup2 gen=");
	puti(gen);
	u_put("\n");
	if (gen != 3)
		fail("lookup2 gen", gen);
	for (i = 0; i < 10; i++)
		echo_one((unsigned)i);
	u_put("UIPCLD: PASS echo2=10/10\n");

	/* Phase 7: STOP. */
	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = 0x3400;
	msg.size = UIPC_HDR_SIZE + 8u;
	rc = uipc_rpc_user("tools", &msg,
			   UIPC_HDR_SIZE + 8u, RPC_TIMEOUT_MS);
	if (rc != (int)(UIPC_HDR_SIZE + 8u) ||
	    msg.body[0] != 'O' || msg.body[1] != 'K')
		fail("stop", rc);
	u_put("UIPCLD: DONE\n");
	sys_exit(0);
}
