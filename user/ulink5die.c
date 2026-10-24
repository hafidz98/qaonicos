/*
 * ulink5die.c - C6L-5 client-death probe (test-only).
 *
 * A spawn child that sends ONE raw (non-RPC) CMD
 * request to `link` with its own reply port, then
 * exits immediately -- without waiting for the
 * reply. linkd must execute the wire exchange
 * anyway and its reply to the dead send-once name
 * must fail harmlessly (logged, not fatal), while
 * other clients (ulink5d, the shell `link`
 * builtin) keep working.
 *
 * NOT in uprogs[]: test-only, "embed:ulink5die".
 * The script runs it against a peer with a delay
 * (--delay-ms), so the request is genuinely in
 * flight when this program exits.
 */
#include "ulib/uipc.h"
#include "uartproto/linkd.h"

static struct uipc_msg	msg;

__attribute__((section(".text.start")))
void
_start(void)
{
	unsigned port;
	int dest, rc;

	rc = uipc_port_alloc();
	if (rc < 0) {
		u_put("ULINK5DIE: FAIL alloc\n");
		sys_exit(1);
	}
	port = (unsigned)rc;
	dest = uipc_lookup("link");
	if (dest < 0) {
		u_put("ULINK5DIE: FAIL lookup\n");
		sys_exit(1);
	}
	msg.reply = port;
	msg.dest = (unsigned)dest;
	msg.id = (int)LINKD_OP_CMD;
	msg.size = UIPC_HDR_SIZE + 16u;
	msg.body[0] = (unsigned char)(LINKD_OP_CMD & 0xFFu);
	msg.body[1] = 0u;
	msg.body[2] = 0u;
	msg.body[3] = 0u;
	msg.body[4] = 0u;	/* mode: plain */
	msg.body[5] = 0u;
	msg.body[6] = 0u;
	msg.body[7] = 0u;
	{
		const char *line = "TIME.GET";
		unsigned i = 0u;

		while (line[i]) {
			msg.body[8u + i] = (unsigned char)line[i];
			i++;
		}
		msg.body[8u + i] = 0u;
		msg.size = UIPC_HDR_SIZE + 8u + i + 1u;
	}
	rc = uipc_send((unsigned)dest, &msg, msg.size);
	if (rc != 0) {
		u_put("ULINK5DIE: FAIL send\n");
		sys_exit(1);
	}
	u_put("ULINK5DIE: sent\n");
	sys_exit(0);
}
