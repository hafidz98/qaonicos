/*
 * uipknd.c - IPC-2 no-deadline park probe (embed child).
 *
 * Parks in uipc_recv with timeout -1 (no deadline) on
 * a private port nobody sends to. Per the plan this
 * form is allowed but BOOTLOGGED: the kernel prints a
 * line and appends bootlog event 17 when the park is
 * taken. Nothing in the harness can wake this child --
 * no second IPC actor exists while it holds the spawn
 * slot -- so the verify script observes the park from
 * outside (kernel line, watchdog silence, GUI still
 * rendering when the GUI is up) and then kills QEMU;
 * the bootlog event is checked on the next boot (the
 * ring persists on the blk device). The return path
 * below exists for completeness: any wake prints the
 * verdict and exits 0 on a real message, 1 otherwise.
 *
 * NOT in uprogs[]: test-only, "embed:uipknd".
 */
#include "ulib/uipc.h"

static struct uipc_msg	msg;

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
	u_put("UIPKND: FAIL ");
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
	int rc;
	unsigned port;

	rc = uipc_port_alloc();
	if (rc < 0)
		fail("alloc", rc);
	port = (unsigned)rc;

	u_put("UIPKND: parking no-deadline\n");
	rc = uipc_recv(port, &msg, UIPC_MSG_MAX, -1);
	u_put("UIPKND: woke rc=");
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
	u_put("\n");
	(void)uipc_port_destroy(port);
	sys_exit(rc >= 0 ? 0 : 1);
}
