/*
 * uipc.h - QaonicOS Mach IPC user API, v0 (IPC-1).
 *
 * Poll-only inline message passing over real Mach ports,
 * through the extension-layer glue (mach3-port/src/ext/
 * ipc_glue.c). The constants and the message layout in
 * this header are MIRRORED BY HAND in ipc_glue.c (the
 * house "copied manually" convention, cf. qaon_stat) --
 * change both or neither.
 *
 * A message is a 24-byte header plus a flat byte body.
 * The header mirrors mach_msg_header_t field for field
 * so the glue can hand it to the real Mach copyin/
 * copyout machinery; userspace never sees Mach's bits
 * word.
 *
 *   send:  hdr.reply = name of a reply port in the
 *          SENDER's space whose receive right the
 *          sender holds (0 = no reply wanted). The glue
 *          turns it into a send-once right for the
 *          receiver (MACH_MSG_TYPE_MAKE_SEND_ONCE).
 *          hdr.id is carried verbatim. hdr.size is the
 *          total message size (header + body).
 *   recv:  hdr.reply = name of the send-once reply
 *          right in the RECEIVER's space (0 = the
 *          sender wanted no reply); answer it with
 *          uipc_reply(). hdr.dest = name of the port
 *          the message arrived on. hdr.size is the
 *          actual total size.
 *
 * All calls are poll-only in v0: nothing ever blocks.
 * A send to a full queue and a receive on an empty
 * queue return an error code immediately (blocking
 * receive is IPC-2; port lifecycle on daemon death is
 * IPC-3; the name registry that replaces the test
 * rendezvous below is IPC-4).
 *
 * Port names are opaque unsigned handles in the
 * caller's own IPC space. Function returns are >= 0 on
 * success (a name, a byte count, or 0) and one of the
 * negative UIPC_E* codes on failure. (House ABI wart,
 * shared with the other syscalls: a port name with the
 * top bit set would alias an error code. Names are
 * table indices + generation in the low bits; at IPC
 * table sizes in play here the alias is unreachable.)
 */
#ifndef QAON_UIPC_H
#define QAON_UIPC_H

#include "ulib/ulib.h"

/* Syscall numbers (mirror mach3-port/src/ext/user.c). */
#define SYS_SEND	10u	/* (dest, buf, len, flags) */
#define SYS_RECV	11u	/* (port, buf, max) -> len */
/* 12 SYS_RPC and 23 SYS_RPC_USER stay reserved (IPC-4). */
#define SYS_IPC_PORT	25u	/* (sub, a1, a2) port mgmt */

/* SYS_SEND flags (a3). */
#define UIPC_F_SEND_ONCE	1u	/* dest is a send-once right:
					 * move it (reply path) */

/* SYS_IPC_PORT sub-ops (a0). */
#define IPCP_ALLOC_RECV	0u	/* -> new receive port name */
#define IPCP_DESTROY	1u	/* (name) drop all rights to name;
				 * a receive name destroys the port */
#define IPCP_SETQLIMIT	2u	/* (name, limit 1..16) -> 0 */
#define IPCP_RV_PUBLISH	3u	/* (slot string, port name):
				 * park the receive right in the
				 * test rendezvous */
#define IPCP_RV_RESOLVE	4u	/* (slot string) -> send name */
#define IPCP_RV_CLAIM	5u	/* (slot string) -> receive name */

/* Message geometry. The body cap is owner-frozen at
 * 4096 B (D-IPC-3, 2026-10-24); out-of-line memory is
 * deferred indefinitely. */
#define UIPC_HDR_SIZE	24u
#define UIPC_BODY_MAX	4096u
#define UIPC_MSG_MAX	(UIPC_HDR_SIZE + UIPC_BODY_MAX)

struct uipc_msg {
	unsigned int	reply;	/* see header comment */
	unsigned int	dest;	/* recv: arrived-on port name */
	int		id;	/* sender-chosen, verbatim */
	unsigned int	size;	/* total size incl. header */
	unsigned int	_pad[2];
	unsigned char	body[UIPC_BODY_MAX];
};

/* Error codes (negative returns). -2 remains the
 * dispatch's ENOSYS for unimplemented numbers and is
 * deliberately not reused here. */
#define UIPC_EINVAL	(-1)	/* bad argument / wrong right kind */
#define UIPC_EDEST	(-3)	/* dead port / no such name or right */
#define UIPC_EFULL	(-4)	/* send: queue full (poll) */
#define UIPC_EEMPTY	(-5)	/* recv: queue empty (poll) */
#define UIPC_E2BIG	(-6)	/* too large; on recv the message
				 * stays queued (Mach semantics) */
#define UIPC_EBUSY	(-7)	/* rendezvous slot conflict */
#define UIPC_ENOMEM	(-8)	/* kernel resource shortage */
#define UIPC_ENORV	(-9)	/* rendezvous slot empty */

/* Port management. */
int	uipc_port_alloc(void);		/* -> name or UIPC_E* */
int	uipc_port_destroy(unsigned name);
int	uipc_port_setqlimit(unsigned name, unsigned limit);

/* Message passing (poll-only). len is the TOTAL size
 * (header + body), body <= UIPC_BODY_MAX. */
int	uipc_send(unsigned dest, const struct uipc_msg *m,
		  unsigned len);
int	uipc_reply(unsigned dest_once, const struct uipc_msg *m,
		   unsigned len);
int	uipc_recv_poll(unsigned port, struct uipc_msg *m,
		       unsigned max);	/* -> total len or UIPC_E* */

/* TEST-ONLY rendezvous (IPC-1). A fixed set of named
 * slots in the glue; PUBLISH parks a receive right,
 * RESOLVE mints a send right for the parked port,
 * CLAIM takes the receive right back into the
 * caller's space. This is scaffolding for the phase's
 * two test programs and is DELETED, not extended, when
 * the IPC-4 registry lands. Do not build product code
 * on it. Slot names are <= 15 chars. */
int	uipc_test_publish(const char *slot, unsigned port);
int	uipc_test_resolve(const char *slot);	/* -> send name */
int	uipc_test_claim(const char *slot);	/* -> recv name */

#endif /* QAON_UIPC_H */
