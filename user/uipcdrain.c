/*
 * uipcdrain.c - IPC-3 test server, drain run (embed-hook
 * spawn child). Claims rendezvous slot "ipc3" and
 * polls its queue once:
 *   - empty -> prints "DRAIN EMPTY" (the fresh-
 *     incarnation proof: no message leaked from a
 *     dead instance's queue into this port);
 *   - one message with id 0xC007 -> prints
 *     "DRAIN MSG OK" (the fresh port genuinely works).
 * Anything else is a FAIL.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipcdrain".
 */
#include "ulib/uipc.h"

static struct uipc_msg msg;

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;
	unsigned port;

	rc = uipc_test_claim("ipc3");
	if (rc < 0) {
		u_put("UIPCDRAIN: FAIL claim\n");
		sys_exit(1);
	}
	port = (unsigned)rc;
	rc = uipc_recv_poll(port, &msg, UIPC_MSG_MAX);
	if (rc == UIPC_EEMPTY) {
		u_put("UIPCDRAIN: DRAIN EMPTY\n");
		sys_exit(0);
	}
	if (rc < 0) {
		u_put("UIPCDRAIN: FAIL recv\n");
		sys_exit(1);
	}
	if (msg.id != 0xC007 || (unsigned)rc != UIPC_HDR_SIZE + 8u ||
	    msg.body[0] != 0xA5u || msg.body[1] != 0x5Au) {
		u_put("UIPCDRAIN: FAIL msg content\n");
		sys_exit(1);
	}
	u_put("UIPCDRAIN: DRAIN MSG OK\n");
	sys_exit(0);
}
