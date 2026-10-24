/*
 * uipc.h - QaonicOS Mach IPC user API, v1 (FROZEN, IPC-4).
 *
 * Inline message passing over real Mach ports,
 * through the extension-layer glue (mach3-port/src/ext/
 * ipc_glue.c). The constants and the message layout in
 * this header are MIRRORED BY HAND in ipc_glue.c (the
 * house "copied manually" convention, cf. qaon_stat) --
 * change both or neither.
 *
 * A message is a 24-byte header plus a flat byte body.
 * The header is the same SIZE as mach_msg_header_t but
 * NOT the same layout: the glue translates it field by
 * field into a real Mach header (bits/size/remote/
 * local/seqno/id) for the copyin/copyout machinery;
 * userspace never sees Mach's bits word.
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
 * Wire size: the glue rounds the total size up to a
 * multiple of 4 (a Mach kmsg requirement) with zero
 * padding, so a receiver can see up to 3 trailing zero
 * bytes beyond the sender's size. Protocols that need
 * the exact length carry it in-band; the test programs
 * use multiple-of-4 body lengths throughout.
 *
 * All send calls are poll-only: a send to a full queue
 * returns an error code immediately. Receive has two
 * forms (IPC-2): uipc_recv_poll() never blocks;
 * uipc_recv(timeout_ms) parks the caller in the kernel
 * scheduler when the queue is empty -- timeout 0 is
 * exactly the poll form, >0 is a deadline in
 * milliseconds, -1 parks without a deadline (allowed
 * but bootlogged by the kernel; the watchdog treats a
 * parked daemon as exempt-but-observed). A deadline
 * expiry returns UIPC_ETIME. Port lifecycle on daemon
 * death is IPC-3.
 *
 * Registry (IPC-4): names live in the kernel, not in
 * any daemon, so the registry survives a registered
 * daemon's death and answers lookups during its
 * respawn window. REGISTER binds a name to the
 * caller's receive port; LOOKUP mints a send right
 * to the registered port and reports the generation
 * separately via REG_GEN. A re-registration after
 * death/respawn bumps the generation; a lookup on a
 * name whose owner died returns UIPC_EDEST (the
 * registry never mints a right to a dead port --
 * upstream ipc_port_make_send() asserts the port
 * active, so minting one would halt the CPU).
 * Reserved system names (below) may be claimed only
 * by the owning program, enforced by the kernel's
 * own spawn identity of the caller slot -- never by
 * a caller-supplied id. A non-owner claim returns
 * UIPC_EPERM and has no side effect.
 *
 * RPC (IPC-4): uipc_rpc() / uipc_rpc_user() send a
 * request carrying a fresh reply port and wait for
 * the reply with the IPC-2 blocking receive. The
 * server answers with uipc_reply() to the send-once
 * name in the request header; a second reply on the
 * same send-once name fails. If the server dies
 * mid-RPC the wait resolves to UIPC_EPEER (IPC-3's
 * peer annotation); a deadline expiry is UIPC_ETIME.
 * A reply larger than the request buffer returns
 * UIPC_E2BIG and the message stays queued -- the
 * caller may then drain it with a plain recv on a
 * bigger buffer (the wrapper destroys its reply port
 * on E2BIG, discarding that queued reply; protocols
 * that can legitimately answer bigger must size the
 * request buffer for the reply).
 *
 * Port names are opaque unsigned handles in the
 * caller's own IPC space. Function returns are >= 0 on
 * success (a name, a byte count, or 0) and one of the
 * negative UIPC_E* codes on failure. (House ABI wart,
 * shared with the other syscalls: a port name with the
 * top bit set would alias an error code. Names are
 * table indices + generation in the low bits; at IPC
 * table sizes in play here the alias is unreachable.)
 *
 * ---- v1 FREEZE (IPC-4, 2026-10-24) ----
 * This header + the syscall/sub-op table below is the
 * version the C6L track builds against. Do not change
 * it without a track decision recorded in
 * docs/rencana-ipc/DEVLOG-IPC4.md.
 *   - inline body cap: 4096 B (D-IPC-3); total 4120 B.
 *   - syscalls: 10 SEND, 11 RECV, 12 RPC, 23 RPC_USER,
 *     25 IPC_PORT (subs 0 ALLOC_RECV, 1 DESTROY,
 *     2 SETQLIMIT, 3 REGISTER, 4 LOOKUP, 5 REG_GEN).
 *   - errors: -1 EINVAL, -2 dispatch ENOSYS (never
 *     reused), -3 EDEST, -4 EFULL, -5 EEMPTY, -6 E2BIG,
 *     -7 EBUSY, -8 ENOMEM, -9 ENORV, -10 ETIME,
 *     -11 EPARK (internal, never to userspace),
 *     -12 EPEER, -13 EPERM.
 *   - reserved names: `tools` (owner: qabotd, the
 *     daemon_images index 2) and `link` (unbound
 *     until C6L-5 binds linkd's index; any claim
 *     refused until then). All other names are open.
 *   - queue sizing and port-set fan-in are IPC-5.
 */
#ifndef QAON_UIPC_H
#define QAON_UIPC_H

#include "ulib/ulib.h"

/* Syscall numbers (mirror mach3-port/src/ext/user.c). */
#define SYS_SEND	10u	/* (dest, buf, len, flags) */
#define SYS_RECV	11u	/* (port, buf, max, timeout_ms) */
#define SYS_RPC		12u	/* (dest, buf, len, timeout_ms):
				 * request in, reply out, same
				 * buffer; reply port is
				 * hdr.reply (0 = allocated by
				 * the uipc_rpc wrapper) */
#define SYS_RPC_USER	23u	/* (name string, buf, len,
				 * timeout_ms): registry lookup
				 * then the SYS_RPC engine */
#define SYS_IPC_PORT	25u	/* (sub, a1, a2) port mgmt */

/* SYS_SEND flags (a3). */
#define UIPC_F_SEND_ONCE	1u	/* dest is a send-once right:
					 * move it (reply path) */

/* SYS_IPC_PORT sub-ops (a0). Subs >= 3 take a name
 * string in a1 (kernel-copied, <= 15 chars). */
#define IPCP_ALLOC_RECV	0u	/* -> new receive port name */
#define IPCP_DESTROY	1u	/* (name) drop all rights to name;
				 * a receive name destroys the port */
#define IPCP_SETQLIMIT	2u	/* (name, limit 1..16) -> 0 */
#define IPCP_REGISTER	3u	/* (name string, port name):
				 * bind the name to the caller's
				 * receive port -> 0 */
#define IPCP_LOOKUP	4u	/* (name string) -> send name */
#define IPCP_REG_GEN	5u	/* (name string) -> generation */

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
#define UIPC_EBUSY	(-7)	/* registry: live entry owned by a
				 * different caller, or table full
				 * of live entries */
#define UIPC_ENOMEM	(-8)	/* kernel resource shortage */
#define UIPC_ENORV	(-9)	/* registry: name never registered */
#define UIPC_ETIME	(-10)	/* blocking recv: deadline expired
				 * with the queue still empty */
#define UIPC_EPARK	(-11)	/* INTERNAL park/wake handshake:
				 * the raw SYS_RECV stub returns it
				 * when a blocking call was parked and
				 * then woken; uipc_recv() loops on it
				 * and never returns it to callers */
#define UIPC_EPEER	(-12)	/* IPC-3: blocking recv: the peer
				 * port died while (or before) the
				 * wait — distinct from UIPC_ETIME.
				 * A wait is peer-annotated iff the
				 * caller sent a reply-carrying
				 * request and then parks on that
				 * request's reply port; a queued
				 * message still beats it (poll
				 * first), and the annotation is
				 * released at wait resolution or
				 * at the waiter's death. */
#define UIPC_EPERM	(-13)	/* IPC-4: registry: reserved name
				 * claimed by a non-owner, or an
				 * unbound reserved name claimed
				 * by anyone. No side effect. */

/* Port management. */
int	uipc_port_alloc(void);		/* -> name or UIPC_E* */
int	uipc_port_destroy(unsigned name);
int	uipc_port_setqlimit(unsigned name, unsigned limit);

/* Registry. Names are <= 15 chars. uipc_lookup returns
 * a send name, or a UIPC_E* code; uipc_reg_gen returns
 * the generation (1 for a fresh registration, bumped
 * on every death and every re-registration), or
 * UIPC_ENORV. */
int	uipc_register(const char *name, unsigned port);
int	uipc_lookup(const char *name);	/* -> send name */
int	uipc_reg_gen(const char *name);	/* -> generation */

/* Message passing. len is the TOTAL size (header +
 * body), body <= UIPC_BODY_MAX. Sends never block.
 * uipc_recv_poll is IPC-1's form (timeout 0).
 * uipc_recv is the IPC-2 default: timeout_ms 0 = poll,
 * >0 = park up to that many milliseconds, -1 = park
 * without a deadline (bootlogged). -> total len or
 * UIPC_E* (UIPC_ETIME on deadline expiry). */
int	uipc_send(unsigned dest, const struct uipc_msg *m,
		  unsigned len);
int	uipc_reply(unsigned dest_once, const struct uipc_msg *m,
		   unsigned len);
int	uipc_recv_poll(unsigned port, struct uipc_msg *m,
		       unsigned max);	/* -> total len or UIPC_E* */
int	uipc_recv(unsigned port, struct uipc_msg *m,
		  unsigned max, int timeout_ms);

/* RPC. The wrapper allocates a fresh reply port, sets
 * hdr.reply, and destroys the reply port on every
 * return path (including EPEER/ETIME/E2BIG), so a
 * caller may loop RPCs without leaking ports. The
 * reply is written over the request in the same
 * buffer; len is the request length and the reply
 * budget. timeout_ms uses the SYS_RECV encoding (0 =
 * poll, >0 = deadline ms, -1 = no deadline).
 * uipc_rpc_user looks the destination up in the
 * registry first. A delivered notify id 71 (Mach
 * send-once death notice) is mapped to UIPC_EPEER.
 * Reply ids must never be 71. */
int	uipc_rpc(unsigned dest, struct uipc_msg *m,
		 unsigned len, int timeout_ms);
int	uipc_rpc_user(const char *name, struct uipc_msg *m,
		      unsigned len, int timeout_ms);

#endif /* QAON_UIPC_H */
