/*
 * uipcregfull.c - IPC-4 registry table-full leg
 * (embed-hook spawn child). Registers 8 names (the
 * whole table) and expects the 9th to fail with
 * UIPC_EBUSY. Runs last in the boot-1 probe: earlier
 * dead entries (`myname`, `gone`) are evicted by
 * design, so 8 live registrations must succeed even
 * though the table already holds dead entries.
 *
 * NOT in uprogs[]: test-only, "embed:uipcregfull".
 */
#include "ulib/uipc.h"

static char	namebuf[4];

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
	u_put("UIPCREGFULL: FAIL ");
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

__attribute__((section(".text.start")))
void
_start(void)
{
	int i, rc, port;

	for (i = 0; i < 8; i++) {
		namebuf[0] = 'r';
		namebuf[1] = (char)('0' + i);
		namebuf[2] = '\0';
		rc = uipc_port_alloc();
		if (rc < 0)
			fail("alloc", rc);
		port = rc;
		rc = uipc_register(namebuf, (unsigned)port);
		if (rc != 0)
			fail("register", rc);
	}
	u_put("UIPCREGFULL: ok 8/8\n");
	namebuf[0] = 'r';
	namebuf[1] = '8';
	namebuf[2] = '\0';
	rc = uipc_port_alloc();
	if (rc < 0)
		fail("alloc9", rc);
	port = rc;
	rc = uipc_register(namebuf, (unsigned)port);
	u_put("UIPCREGFULL: ninth rc=");
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
	u_put("\n");
	if (rc != UIPC_EBUSY)
		fail("ninth", rc);
	u_put("UIPCREGFULL: PASS\n");
	sys_exit(0);
}
