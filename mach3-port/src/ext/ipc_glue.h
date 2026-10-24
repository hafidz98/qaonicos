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
#define UIPC_EPEER	(-12)	/* IPC-3: RPC peer died while waiting */

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
/* IPC-3: send variant that also reports the destination
 * port OBJECT (an ipc_port_t, opaque void *) and the
 * reply port NAME from the message header, on success
 * only (NULL out-params are ignored). The dispatch
 * uses them for the peer annotation: a reply-carrying
 * COPY_SEND makes the sender's next park on that reply
 * port a peer-annotated wait. The object is reported
 * WITHOUT a reference; the dispatch takes one when it
 * stores the annotation (same trap context, so nothing
 * can free the object in between). */
extern unsigned int ipc_glue_send_ex(task_t who, unsigned int dest,
				     const void *ubuf, unsigned int len,
				     unsigned int flags,
				     void **dest_obj_out,
				     unsigned int *reply_out);
/* IPC-3: IO_BITS_ACTIVE test on a pinned object (a
 * destroyed port has the bit cleared but is not freed
 * while a reference is held, so this is safe on any
 * object the caller holds a ref on). */
extern int ipc_glue_port_alive(void *obj);
extern void ipc_glue_take_ref(void *obj);
extern void ipc_glue_release_ref(void *obj);
/* IPC-3: destroy every right in a dead task's space:
 * receive ports first (their queues are discarded by
 * upstream port destruction, and parked RPC waiters
 * annotated with the port are woken peer-died), then
 * all remaining entries. Reports the number of rights
 * destroyed and queued messages discarded (NULL
 * out-params ignored). The task object, its vm_map
 * and its pmap are NOT freed: that is the pre-existing
 * task leak (see DEVLOG-IPC3), unchanged by IPC-3. */
extern void ipc_glue_teardown(task_t who, unsigned int *ports_out,
			      unsigned int *msgs_out);

#endif /* QAON_IPC_GLUE_H */
