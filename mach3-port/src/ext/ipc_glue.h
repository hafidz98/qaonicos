/*
 * ipc_glue.h - IPC-1 extension-layer glue: entry points
 * called from the syscall dispatch in user.c.
 *
 * The contract on the user side is user/ulib/uipc.h
 * (message layout, sub-ops, error codes); the constants
 * below mirror it BY HAND, the house convention.
 */
#ifndef QAON_IPC_GLUE_H
#define QAON_IPC_GLUE_H

#include <kern/task.h>		/* task_t */

/* Mirrors of user/ulib/uipc.h. */
#define UIPC_HDR_SIZE	24u
#define UIPC_BODY_MAX	4096u	/* D-IPC-3: inline cap */
#define UIPC_MSG_MAX	(UIPC_HDR_SIZE + UIPC_BODY_MAX)

#define UIPC_EINVAL	(-1)
#define UIPC_EDEST	(-3)
#define UIPC_EFULL	(-4)
#define UIPC_EEMPTY	(-5)
#define UIPC_E2BIG	(-6)
#define UIPC_EBUSY	(-7)
#define UIPC_ENOMEM	(-8)
#define UIPC_ENORV	(-9)
#define UIPC_ETIME	(-10)	/* IPC-2: blocking deadline expiry */
#define UIPC_EPARK	(-11)	/* IPC-2: internal park/wake code */

#define UIPC_F_SEND_ONCE	1u

/* SYS_IPC_PORT sub-ops (mirror uipc.h IPCP_*). */
#define IPCP_ALLOC_RECV	0u
#define IPCP_DESTROY	1u
#define IPCP_SETQLIMIT	2u
#define IPCP_RV_PUBLISH	3u
#define IPCP_RV_RESOLVE	4u
#define IPCP_RV_CLAIM	5u

/* SYS_SEND: (who, dest name, user buf, total len, flags).
 * SYS_RECV: (who, port name, user buf, buffer max).
 * SYS_IPC_PORT: (who, sub, kname, x, y) -- for the RV_*
 * subs kname is the kernel-copied slot-name string and x
 * is the port name (PUBLISH); for the plain subs kname
 * is NULL, x is the port name and y the qlimit
 * (SETQLIMIT). Returns are >= 0 on success (a byte count
 * or a port name) or a UIPC_E* code, as unsigned. */
extern unsigned int ipc_glue_send(task_t who, unsigned int dest,
				  const void *ubuf, unsigned int len,
				  unsigned int flags);
extern unsigned int ipc_glue_recv(task_t who, unsigned int port,
				  void *ubuf, unsigned int max);
/* IPC-2: recv variant that also reports the destination
 * port OBJECT identity (an ipc_port_t, kept opaque as
 * void *) of the receive port -- on every outcome where
 * the name resolved, including UIPC_EEMPTY. The
 * scheduler matches later sends against this identity
 * to wake a daemon parked on the port. The port stays
 * alive while the caller's space holds its receive
 * right, which is the whole life of a wait. */
extern unsigned int ipc_glue_recv_ex(task_t who, unsigned int port,
				     void *ubuf, unsigned int max,
				     void **obj_out);
extern unsigned int ipc_glue_port(task_t who, unsigned int sub,
				  const char *kname, unsigned int x,
				  unsigned int y);

#endif /* QAON_IPC_GLUE_H */
