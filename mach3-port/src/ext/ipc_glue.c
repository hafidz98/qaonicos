/*
 * ipc_glue.c - IPC-1: poll-only inline Mach IPC, glued
 * from the extension layer (plan: docs/rencana-ipc,
 * research: docs/riset-ipc-mach).
 *
 * The Mach IPC machinery is fully compiled into this
 * kernel (upstream ipc sources, 17 of 18 files) and
 * initialised every boot (ipc_bootstrap/ipc_init), and
 * every daemon already owns a real task_t with a real
 * ipc_space (task_create -> ipc_task_init). What was
 * missing is exactly this file: a caller.
 *
 * IDENTITY. The dispatch (user.c) hands us the caller's
 * task (cur_utask -- the task of the daemon/spawn slot
 * actually running). Every Mach operation is performed
 * FOR that task: names are interpreted in its
 * itk_space, copyin/copyout use its vm_map. We never
 * call mach_msg_trap: it derives identity from
 * current_thread(), which in this port is always the
 * boot thread (the extension scheduler multiplexes all
 * daemons onto one Mach thread), so the trap would
 * silently operate on the kernel task's space. The
 * layer beneath the trap takes identity explicitly --
 * that is the layer used here.
 *
 * NO BLOCKING IN THE GLUE. ipc_mqueue_send/receive
 * block via thread_block() on current_thread() --
 * meaningless under the extension scheduler -- but both
 * have a native timeout-0 mode that returns *_TIMED_OUT
 * without reaching the blocking branch (verified in
 * upstream ipc/ipc_mqueue.c: the empty-queue receive
 * checks `time_out == 0` before any thread_will_wait,
 * and the full-queue send likewise). The glue passes
 * timeout 0 ALWAYS; polling lives in userspace. The
 * sanctioned blocking receive (D-IPC-1) is a scheduler
 * switch point in user.c (IPC-2), built on this file:
 * recv_ex reports the port-object identity a wait
 * parks on, and send wakes parked receivers through
 * sched_ipc_wake_port() after staging.
 *
 * MESSAGE SHAPE. Userspace sees the 24-byte uipc.h
 * header; the glue translates it field by field into a
 * mach_msg_header_t in a static kernel buffer (syscalls
 * are serial under the cooperative scheduler -- the
 * SYS_SPAWN imgbuf / BOOTSTATE pkgbuf pattern), builds
 * a kmsg with ipc_kmsg_get_from_kernel, and lets the
 * real copyin machinery translate the rights: dest is
 * COPY_SEND (or MOVE_SEND_ONCE on the reply path),
 * the header reply field is MAKE_SEND_ONCE. The
 * COMPLEX bit is never set, so copyin/copyout process
 * the header only and body bytes are never parsed --
 * there is no descriptor interpretation anywhere.
 * The wire size is rounded up to a multiple of 4 (a
 * kmsg requirement) with zero padding.
 *
 * Not built here, by plan: port/space teardown on
 * daemon death (IPC-3), the name registry (IPC-4 --
 * the test rendezvous below is scaffolding that IPC-4
 * deletes, not extends).
 * Out-of-line memory is out of scope indefinitely
 * (D-IPC-3); the inline body cap is 4096 B.
 */

#include <mach/message.h>
#include <mach/port.h>
#include <mach/kern_return.h>
#include <kern/task.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_kmsg.h>
#include <ipc/ipc_mqueue.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_kmsg.h>
#include <ipc/port.h>
#include "ipc_glue.h"

/* MIG-generated prototypes are not part of the ext
 * include set; declare the entry points by hand (the
 * house pattern, cf. user.c's extern block). */
extern mach_msg_return_t ipc_mqueue_copyin(ipc_space_t, mach_port_t,
					   ipc_mqueue_t *,
					   ipc_object_t *);
extern kern_return_t mach_port_allocate(ipc_space_t,
					mach_port_right_t,
					mach_port_t *);
extern kern_return_t mach_port_destroy(ipc_space_t, mach_port_t);
extern kern_return_t mach_port_set_qlimit(ipc_space_t, mach_port_t,
					  mach_port_msgcount_t);
extern kern_return_t mach_port_extract_right(ipc_space_t, mach_port_t,
					     mach_msg_type_name_t,
					     ipc_object_t *,
					     mach_msg_type_name_t *);

/* IPC-2: scheduler wake hook (user.c). Called after a
 * message has been STAGED on a port: any daemon parked
 * in a blocking receive on that port object becomes
 * runnable again (D-IPC-1: the wake restarts its
 * watchdog progress clock). */
extern void sched_ipc_wake_port(void *port_obj);
/* IPC-3: scheduler peer-death hook (user.c). Called
 * after a port OBJECT has been destroyed: any waiter
 * whose wait record is peer-annotated with that object
 * is woken with the peer-died verdict (its re-dispatch
 * returns UIPC_EPEER, never a message or a deadline). */
extern void sched_ipc_wake_peer(void *port_obj);

/* Userspace message header (mirror of struct uipc_msg's
 * first 24 bytes in user/ulib/uipc.h). */
struct uipc_hdr {
	unsigned int	reply;
	unsigned int	dest;
	int		id;
	unsigned int	size;
	unsigned int	_pad[2];
};

/* One static message buffer: syscalls are serial in
 * the cooperative model, so no syscall can observe
 * another's half-built message. */
static struct {
	mach_msg_header_t	hdr;
	unsigned char		body[UIPC_BODY_MAX + 4u];
} ipc_msgbuf;

static int
ipc_space_of(task_t who, ipc_space_t *spacep)
{
	if (who == TASK_NULL || who->itk_space == IS_NULL)
		return 0;
	*spacep = who->itk_space;
	return 1;
}

static int
map_send_err(mach_msg_return_t mr)
{
	switch (mr) {
	case MACH_SEND_INVALID_DEST:
	case MACH_SEND_INVALID_RIGHT:
		return UIPC_EDEST;
	case MACH_SEND_TIMED_OUT:
		return UIPC_EFULL;
	case MACH_SEND_NO_BUFFER:
		return UIPC_ENOMEM;
	default:
		return UIPC_EINVAL;
	}
}

unsigned int
ipc_glue_send(task_t who, unsigned int dest, const void *ubuf,
	      unsigned int len, unsigned int flags)
{
	return ipc_glue_send_ex(who, dest, ubuf, len, flags,
				(void **)0, (unsigned int *)0);
}

unsigned int
ipc_glue_send_ex(task_t who, unsigned int dest, const void *ubuf,
		 unsigned int len, unsigned int flags,
		 void **dest_obj_out, unsigned int *reply_out)
{
	const struct uipc_hdr *uh = (const struct uipc_hdr *)ubuf;
	const unsigned char *ubody =
	    (const unsigned char *)ubuf + UIPC_HDR_SIZE;
	ipc_space_t space;
	ipc_kmsg_t kmsg;
	mach_msg_return_t mr;
	unsigned int wire, i, blen;

	if (!ipc_space_of(who, &space))
		return (unsigned int)UIPC_EINVAL;
	if (len < UIPC_HDR_SIZE)
		return (unsigned int)UIPC_EINVAL;
	if (len > UIPC_MSG_MAX)
		return (unsigned int)UIPC_E2BIG;
	blen = len - UIPC_HDR_SIZE;
	wire = (len + 3u) & ~3u;

	for (i = 0; i < blen; i++)
		ipc_msgbuf.body[i] = ubody[i];
	for (; i < wire - UIPC_HDR_SIZE; i++)
		ipc_msgbuf.body[i] = 0;

	ipc_msgbuf.hdr.msgh_size = wire;
	ipc_msgbuf.hdr.msgh_remote_port = (mach_port_t)dest;
	if (flags & UIPC_F_SEND_ONCE) {
		/* Reply path: move the send-once right; a
		 * reply carries no further reply right in v1. */
		ipc_msgbuf.hdr.msgh_bits =
		    MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
		ipc_msgbuf.hdr.msgh_local_port = MACH_PORT_NULL;
	} else {
		ipc_msgbuf.hdr.msgh_bits =
		    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
				   uh->reply != 0u ?
				   MACH_MSG_TYPE_MAKE_SEND_ONCE : 0);
		ipc_msgbuf.hdr.msgh_local_port = (mach_port_t)uh->reply;
	}
	ipc_msgbuf.hdr.msgh_seqno = 0;
	ipc_msgbuf.hdr.msgh_id = uh->id;

	mr = ipc_kmsg_get_from_kernel(&ipc_msgbuf.hdr, wire, &kmsg);
	if (mr != MACH_MSG_SUCCESS)
		return (unsigned int)UIPC_ENOMEM;
	mr = ipc_kmsg_copyin(kmsg, space, who->map, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		ipc_kmsg_free(kmsg);
		return (unsigned int)map_send_err(mr);
	}
	/* IPC-2: copyin translated the destination name
	 * into the destination port OBJECT (it sits in the
	 * kmsg header's remote-port field). Capture it
	 * before the send consumes the kmsg: after a
	 * successful send the scheduler matches parked
	 * receivers against exactly this identity. */
	{
		void *dest_obj = (void *)kmsg->ikm_header.msgh_remote_port;

		/* Timeout 0, always: a full queue is
		 * UIPC_EFULL, never a block (see the header
		 * comment). */
		mr = ipc_mqueue_send(kmsg, MACH_SEND_TIMEOUT, 0);
		if (mr != MACH_MSG_SUCCESS) {
			ipc_kmsg_destroy(kmsg);
			return (unsigned int)map_send_err(mr);
		}
		/* IPC-3: report the staged destination for the
		 * dispatch's peer annotation (success only;
		 * no reference is taken here -- see the header
		 * comment). */
		if (dest_obj_out != (void **)0)
			*dest_obj_out = dest_obj;
		if (reply_out != (unsigned int *)0)
			*reply_out = uh->reply;
		sched_ipc_wake_port(dest_obj);
	}
	return 0;
}

static unsigned int
recv_core(task_t who, unsigned int port, void *ubuf,
	  unsigned int max, void **obj_out)
{
	struct uipc_hdr *uh = (struct uipc_hdr *)ubuf;
	unsigned char *ubody = (unsigned char *)ubuf + UIPC_HDR_SIZE;
	ipc_space_t space;
	ipc_mqueue_t mqueue;
	ipc_object_t object;
	ipc_kmsg_t kmsg;
	mach_port_seqno_t seqno;
	mach_msg_return_t mr;
	unsigned int total, i;

	if (!ipc_space_of(who, &space))
		return (unsigned int)UIPC_EINVAL;
	if (max < UIPC_HDR_SIZE)
		return (unsigned int)UIPC_E2BIG;

	mr = ipc_mqueue_copyin(space, (mach_port_t)port,
			       &mqueue, &object);
	if (mr != MACH_MSG_SUCCESS)
		return (unsigned int)UIPC_EDEST;
	/* IPC-2: hand the port-object identity to the
	 * caller before the receive attempt, so the
	 * UIPC_EEMPTY path (the one that parks) has it too.
	 * A raw pointer is a safe identity for a wait's
	 * whole life: the receive right lives in the
	 * waiter's own space for exactly that long, so the
	 * port cannot be freed underneath the park. */
	if (obj_out != (void **)0)
		*obj_out = (void *)object;
	/* Timeout 0, always: an empty queue is UIPC_EEMPTY,
	 * never a block. MACH_RCV_LARGE semantics: a
	 * message bigger than the buffer stays queued. */
	mr = ipc_mqueue_receive(mqueue,
				MACH_RCV_TIMEOUT | MACH_RCV_LARGE,
				max, 0, FALSE, IMQ_NULL_CONTINUE,
				&kmsg, &seqno);
	ipc_object_release(object);
	if (mr == MACH_RCV_TIMED_OUT)
		return (unsigned int)UIPC_EEMPTY;
	if (mr == MACH_RCV_TOO_LARGE)
		return (unsigned int)UIPC_E2BIG;
	if (mr != MACH_MSG_SUCCESS)
		return (unsigned int)UIPC_EINVAL;

	/* Translate the header into the receiver's space:
	 * this inserts the reply send-once right under a
	 * name (or MACH_PORT_NULL) and names the port the
	 * message arrived on. Body bytes are not parsed
	 * (the COMPLEX bit is never set by our senders). */
	mr = ipc_kmsg_copyout(kmsg, space, who->map, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		ipc_kmsg_destroy(kmsg);
		return (unsigned int)UIPC_EINVAL;
	}
	total = kmsg->ikm_header.msgh_size;
	uh->reply = (unsigned int)kmsg->ikm_header.msgh_remote_port;
	uh->dest = (unsigned int)kmsg->ikm_header.msgh_local_port;
	uh->id = kmsg->ikm_header.msgh_id;
	uh->size = total;
	{
		const unsigned char *kbody =
		    (const unsigned char *)(&kmsg->ikm_header + 1);
		for (i = 0; i + UIPC_HDR_SIZE < total; i++)
			ubody[i] = kbody[i];
	}
	ipc_kmsg_free(kmsg);
	return total;
}

unsigned int
ipc_glue_recv(task_t who, unsigned int port, void *ubuf,
	      unsigned int max)
{
	return recv_core(who, port, ubuf, max, (void **)0);
}

unsigned int
ipc_glue_recv_ex(task_t who, unsigned int port, void *ubuf,
		 unsigned int max, void **obj_out)
{
	return recv_core(who, port, ubuf, max, obj_out);
}

/* ---------------- IPC-3: lifecycle ----------------
 * A dead daemon's ports must die with it, or senders
 * queue messages on a corpse port forever (the IPC-1/2
 * behaviour this phase fixes). The teardown below is
 * SURGICAL: it destroys rights in the dead task's
 * space, which is every IPC-visible effect of a full
 * task_terminate (empty space, dead senders, discarded
 * queues, dead-name on next send) -- see DEVLOG-IPC3
 * for the evidence. The task object itself is not
 * freed (pre-existing leak, bounded by the FND-3
 * respawn budget for persistent daemons).
 *
 * Single-threaded trap context: no other task can
 * mutate this space while teardown runs, so the table
 * walk needs no extra locking (mach_port_destroy
 * handles space locking internally). Destroying an
 * entry never shrinks the table or moves other
 * entries, so iterating by index while destroying is
 * safe; freed entries join the free list.
 */
int
ipc_glue_port_alive(void *obj)
{
	if (obj == (void *)0)
		return 0;
	return io_active((ipc_object_t)obj);
}

void
ipc_glue_take_ref(void *obj)
{
	if (obj != (void *)0)
		ipc_object_reference((ipc_object_t)obj);
}

void
ipc_glue_release_ref(void *obj)
{
	if (obj != (void *)0)
		ipc_object_release((ipc_object_t)obj);
}

/* Type of a name in a space, by table lookup (the
 * generation must match, or the name is stale). 0 =
 * no such entry. Used by the DESTROY hook to tell a
 * receive right (whose destruction kills the port)
 * from a send right (whose destruction kills nothing). */
static unsigned int
space_entry_type(ipc_space_t space, unsigned int name)
{
	unsigned int idx = MACH_PORT_INDEX(name);
	ipc_entry_t entry;

	if (idx >= space->is_table_size)
		return 0u;
	entry = &space->is_table[idx];
	if (entry->ie_object == IP_NULL)
		return 0u;
	if (MACH_PORT_GEN(name) != IE_BITS_GEN(entry->ie_bits))
		return 0u;
	return IE_BITS_TYPE(entry->ie_bits);
}

void
ipc_glue_teardown(task_t who, unsigned int *ports_out,
		  unsigned int *msgs_out)
{
	ipc_space_t space;
	unsigned int i, ports = 0u, msgs = 0u;

	if (ports_out != (unsigned int *)0)
		*ports_out = 0u;
	if (msgs_out != (unsigned int *)0)
		*msgs_out = 0u;
	if (!ipc_space_of(who, &space))
		return;
	/* Our spaces never grow a tree: a handful of
	 * sequential allocations in the 128-entry initial
	 * table. If this ever fires, teardown is
	 * incomplete by exactly the tree entries. */
	if (space->is_tree_total != 0u)
		printf("ipc: teardown: WARNING %u tree entries "
		    "NOT torn down\n", space->is_tree_total);
	/* Pass 1: receive rights. Destroying one kills
	 * its port: upstream ipc_port_destroy discards
	 * the queue (counted first, bounded walk of the
	 * circular kmsg list) and wakes no-senders
	 * requesters (none registered in our flows).
	 * Receive-first ordering is what makes senders
	 * go dead immediately. The object is pinned
	 * across the destroy so the peer-wake comparison
	 * below cannot see a recycled pointer. */
	for (i = 0u; i < space->is_table_size; i++) {
		ipc_entry_t entry = &space->is_table[i];
		ipc_port_t port;
		mach_port_t name;
		ipc_kmsg_t k, base;
		unsigned int n = 0u;

		if (entry->ie_object == IP_NULL)
			continue;
		/* Mask, not equality: a send-right copyout
		 * for a port the task already holds
		 * receive for coalesces into the existing
		 * entry (observed: sh's lookup of its own
		 * registered name returned the receive
		 * name itself), so the bits can read
		 * RECEIVE|SEND. An entry holding receive
		 * rights IS a receive right. The equality
		 * test silently skipped such entries here
		 * (no peer-wake, no registry death note)
		 * and in the DESTROY hook and REGISTER
		 * validation -- the IPC-4 regfull failure
		 * was a live entry the destroy hook had
		 * failed to mark dead. */
		if ((IE_BITS_TYPE(entry->ie_bits) &
		     MACH_PORT_TYPE_RECEIVE) == 0)
			continue;
		port = (ipc_port_t)entry->ie_object;
		name = MACH_PORT_MAKEB(i, entry->ie_bits);
		base = port->ip_messages.imq_messages.ikmq_base;
		if (base != IKM_NULL) {
			k = base;
			do {
				n++;
				k = k->ikm_next;
			} while (k != base && n < 4096u);
		}
		msgs += n;
		ipc_object_reference((ipc_object_t)port);
		if (mach_port_destroy(space, name) == KERN_SUCCESS)
			ports++;
		sched_ipc_wake_peer((void *)port);
		/* IPC-4: a destroyed receive port kills its
		 * registry entry (dead, generation bumped),
		 * so lookups during the respawn window
		 * answer EDEST instead of minting a right
		 * to a corpse. The object is pinned across
		 * the destroy, so the comparison is safe. */
		ipc_glue_registry_dead((void *)port);
		ipc_object_release((ipc_object_t)port);
	}
	/* Pass 2: everything else (send, send-once,
	 * dead-name, port-set entries). Every entry
	 * still pointing at an object holds a reference
	 * on it, so destroying these cannot touch a
	 * freed port. */
	for (i = 0u; i < space->is_table_size; i++) {
		ipc_entry_t entry = &space->is_table[i];
		mach_port_t name;

		if (entry->ie_object == IP_NULL)
			continue;
		name = MACH_PORT_MAKEB(i, entry->ie_bits);
		if (mach_port_destroy(space, name) == KERN_SUCCESS)
			ports++;
	}
	if (ports_out != (unsigned int *)0)
		*ports_out = ports;
	if (msgs_out != (unsigned int *)0)
		*msgs_out = msgs;
}

/* ---------------- kernel registry (IPC-4) ----------------
 * name -> port + generation, kernel resident: the
 * registry survives a registered daemon's death and
 * answers lookups during its respawn window (as a
 * truthful UIPC_EDEST for a dead owner, never a live
 * right -- upstream ipc_port_make_send() asserts the
 * port active, so minting a right to a dead port
 * would halt the CPU).
 *
 * Ownership: REGISTER takes the receive right's
 * object (a reference, like any table entry) and
 * records the caller's kernel-assigned progid. A
 * live entry may be replaced only by the same owner
 * (EBUSY otherwise); a dead entry may be taken over
 * by anyone (reserved names still gated first).
 * Re-registration and death both bump the
 * generation, so a stale send right is detectable
 * (its generation predates the current one) and a
 * stale LOOKUP result is never a live port.
 *
 * Reserved names (D-IPC-2): `tools` belongs to
 * qabotd (daemon_images index 2); `link` is unbound
 * until C6L-5 binds linkd's index, so any claim is
 * refused until then. The check keys on progid, the
 * kernel-assigned spawn identity -- never on a
 * caller-supplied id -- and runs before any table
 * mutation, so a refused claim has no side effect.
 *
 * Eviction: a new name into a full table evicts the
 * oldest DEAD entry (documented: the evicted name's
 * generation resets when it is next registered, since
 * the entry is gone, not bumped). A table full of
 * live entries returns EBUSY. This keeps dead
 * entries from exhausting the table within a boot.
 */
#define REG_SLOTS	8u
#define REG_NAME_MAX	15u
#define REG_OWNER_UNBOUND	(-2)

static struct {
	char		name[REG_NAME_MAX + 1u];
	ipc_port_t	port;	/* IP_NULL = slot free */
	int		owner;	/* kernel progid, -1 = one-shot */
	unsigned int	generation;
	int		alive;
} reg_table[REG_SLOTS];

static const struct {
	const char	*name;
	int		owner;
} reg_reserved[] = {
	{ "tools", 2 },			/* qabotd */
	{ "link", REG_OWNER_UNBOUND },	/* bound at C6L-5 */
};

static int
reg_streq(const char *a, const char *b)
{
	unsigned int i;
	for (i = 0;; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == '\0')
			return 1;
	}
}

static int
reg_find(const char *name)
{
	unsigned int i;
	for (i = 0; i < REG_SLOTS; i++)
		if (reg_table[i].port != IP_NULL &&
		    reg_streq(reg_table[i].name, name))
			return (int)i;
	return -1;
}

/* Reserved-name gate: 0 = allowed, UIPC_EPERM =
 * refused. Runs before any mutation. */
static unsigned int
reg_reserved_check(const char *name, int progid)
{
	unsigned int i;
	for (i = 0; i < sizeof(reg_reserved) / sizeof(reg_reserved[0]);
	     i++) {
		if (!reg_streq(reg_reserved[i].name, name))
			continue;
		if (reg_reserved[i].owner == REG_OWNER_UNBOUND)
			return (unsigned int)UIPC_EPERM;
		if (progid != reg_reserved[i].owner)
			return (unsigned int)UIPC_EPERM;
		return 0;
	}
	return 0;
}

void
ipc_glue_registry_dead(void *obj)
{
	unsigned int i;
	for (i = 0; i < REG_SLOTS; i++) {
		if (reg_table[i].port != IP_NULL &&
		    reg_table[i].port == (ipc_port_t)obj &&
		    reg_table[i].alive) {
			reg_table[i].alive = 0;
			reg_table[i].generation++;
		}
	}
}

unsigned int
ipc_glue_registry(task_t who, int progid, unsigned int sub,
		  const char *name, unsigned int portname)
{
	ipc_space_t space;
	int slot;
	unsigned int i;

	if (name == (const char *)0 || name[0] == '\0')
		return (unsigned int)UIPC_EINVAL;
	if (!ipc_space_of(who, &space))
		return (unsigned int)UIPC_EINVAL;
	switch (sub) {
	case IPCP_REGISTER: {
		ipc_mqueue_t mqueue;
		ipc_object_t object;
		unsigned int rc;

		rc = reg_reserved_check(name, progid);
		if (rc != 0)
			return rc;
		/* Resolve the caller's receive right: copyin
		 * takes a reference, which pins the object
		 * across the register, and returns the
		 * port's message queue LOCKED (the copyin
		 * contract: the lock is for a receive call
		 * that follows). This register performs no
		 * receive, so the queue is unlocked at once;
		 * the reference alone pins the port. A send
		 * name, a dead name or a port set is not a
		 * receive right and is refused. */
		if (ipc_mqueue_copyin(space, (mach_port_t)portname,
				      &mqueue, &object) !=
		    MACH_MSG_SUCCESS)
			return (unsigned int)UIPC_EDEST;
		imq_unlock(mqueue);
		/* Mask test: see the teardown pass-1
		 * comment -- a coalesced RECEIVE|SEND
		 * entry still holds a receive right. */
		if ((space_entry_type(space, portname) &
		     MACH_PORT_TYPE_RECEIVE) == 0) {
			ipc_object_release(object);
			return (unsigned int)UIPC_EINVAL;
		}
		slot = reg_find(name);
		if (slot >= 0) {
			if (reg_table[slot].alive &&
			    reg_table[slot].owner != progid) {
				ipc_object_release(object);
				return (unsigned int)UIPC_EBUSY;
			}
			/* Same owner, or a dead entry taken
			 * over: replace the port (a same-object
			 * re-register keeps its ref) and bump. */
			if (reg_table[slot].port !=
			    (ipc_port_t)object) {
				ipc_object_release(
				    (ipc_object_t)reg_table[slot].port);
				reg_table[slot].port =
				    (ipc_port_t)object;
			} else
				ipc_object_release(object);
			reg_table[slot].owner = progid;
			reg_table[slot].alive = 1;
			reg_table[slot].generation++;
			return 0;
		}
		/* New name: a free slot, else evict the
		 * oldest dead entry. */
		slot = -1;
		for (i = 0; i < REG_SLOTS; i++)
			if (reg_table[i].port == IP_NULL) {
				slot = (int)i;
				break;
			}
		if (slot < 0)
			for (i = 0; i < REG_SLOTS; i++)
				if (!reg_table[i].alive) {
					ipc_object_release(
					    (ipc_object_t)reg_table[i].port);
					reg_table[i].port = IP_NULL;
					reg_table[i].name[0] = '\0';
					slot = (int)i;
					break;
				}
		if (slot < 0) {
			ipc_object_release(object);
			return (unsigned int)UIPC_EBUSY;
		}
		for (i = 0; i <= REG_NAME_MAX; i++) {
			reg_table[slot].name[i] = name[i];
			if (name[i] == '\0')
				break;
		}
		reg_table[slot].port = (ipc_port_t)object;
		reg_table[slot].owner = progid;
		reg_table[slot].generation = 1u;
		reg_table[slot].alive = 1;
		return 0;
	}
	case IPCP_LOOKUP: {
		ipc_port_t sright;
		mach_port_t send_name;

		slot = reg_find(name);
		if (slot < 0)
			return (unsigned int)UIPC_ENORV;
		if (!reg_table[slot].alive)
			return (unsigned int)UIPC_EDEST;
		/* Liveness was just checked in trap
		 * context (no other task can run while
		 * this trap runs), so make_send cannot
		 * hit its active assertion; the NULL
		 * branch is defensive only. */
		sright = ipc_port_make_send(reg_table[slot].port);
		if (sright == IP_NULL)
			return (unsigned int)UIPC_EDEST;
		send_name = ipc_port_copyout_send(sright, space);
		if (send_name == MACH_PORT_NULL)
			return (unsigned int)UIPC_ENOMEM;
		return (unsigned int)send_name;
	}
	case IPCP_REG_GEN:
		slot = reg_find(name);
		if (slot < 0)
			return (unsigned int)UIPC_ENORV;
		return reg_table[slot].generation;
	default:
		return (unsigned int)UIPC_EINVAL;
	}
}

unsigned int
ipc_glue_port(task_t who, int progid, unsigned int sub,
	      const char *kname, unsigned int x, unsigned int y)
{
	ipc_space_t space;
	kern_return_t kr;

	if (!ipc_space_of(who, &space))
		return (unsigned int)UIPC_EINVAL;
	switch (sub) {
	case IPCP_ALLOC_RECV: {
		mach_port_t name;
		kr = mach_port_allocate(space, MACH_PORT_RIGHT_RECEIVE,
					&name);
		if (kr == KERN_RESOURCE_SHORTAGE || kr == KERN_NO_SPACE)
			return (unsigned int)UIPC_ENOMEM;
		if (kr != KERN_SUCCESS)
			return (unsigned int)UIPC_EINVAL;
		return (unsigned int)name;
	}
	case IPCP_DESTROY: {
		/* IPC-3: a live daemon destroying its own
		 * receive port is a peer death for any
		 * waiter annotated with that port (the
		 * shutdown case). Resolve first (copyin
		 * takes a reference, which pins the object
		 * across the destroy), destroy, then wake
		 * peers ONLY if the name really was a
		 * receive right: destroying a send name
		 * kills nothing, and waking on it would
		 * lie to a waiter. Unresolvable names
		 * (send names to dead ports, dead names,
		 * port sets) keep the old behaviour. */
		ipc_mqueue_t mqueue;
		ipc_object_t object = (ipc_object_t)0;
		unsigned int was_recv = 0u;

		if (ipc_mqueue_copyin(space, (mach_port_t)x,
				      &mqueue, &object) ==
		    MACH_MSG_SUCCESS)
			/* Mask test: see the teardown
			 * pass-1 comment. */
			was_recv =
			    ((space_entry_type(space, x) &
			      MACH_PORT_TYPE_RECEIVE) != 0);
		/* Copyin returns the queue locked; this
		 * hook performs no receive, so unlock at
		 * once (the reference pins the object). */
		if (object != (ipc_object_t)0)
			imq_unlock(mqueue);
		kr = mach_port_destroy(space, (mach_port_t)x);
		if (kr == KERN_INVALID_NAME) {
			if (object != (ipc_object_t)0)
				ipc_object_release(object);
			return (unsigned int)UIPC_EDEST;
		}
		if (kr != KERN_SUCCESS) {
			if (object != (ipc_object_t)0)
				ipc_object_release(object);
			return (unsigned int)UIPC_EINVAL;
		}
		if (was_recv && object != (ipc_object_t)0)
			sched_ipc_wake_peer((void *)object);
		if (was_recv && object != (ipc_object_t)0)
			ipc_glue_registry_dead((void *)object);
		if (object != (ipc_object_t)0)
			ipc_object_release(object);
		return 0;
	}
	case IPCP_SETQLIMIT:
		/* Upstream rejects > MACH_PORT_QLIMIT_MAX
		 * (16) with KERN_INVALID_VALUE; mirror the
		 * bound here so the error is ours, and see
		 * DEVLOG-IPC1 for what the cap means. */
		if (y < 1u || y > 16u)
			return (unsigned int)UIPC_EINVAL;
		kr = mach_port_set_qlimit(space, (mach_port_t)x,
					  (mach_port_msgcount_t)y);
		if (kr == KERN_INVALID_NAME)
			return (unsigned int)UIPC_EDEST;
		if (kr != KERN_SUCCESS)
			return (unsigned int)UIPC_EINVAL;
		return 0;
	case IPCP_REGISTER:
	case IPCP_LOOKUP:
	case IPCP_REG_GEN:
		if (kname == (const char *)0)
			return (unsigned int)UIPC_EINVAL;
		return ipc_glue_registry(who, progid, sub, kname, x);
	default:
		return (unsigned int)UIPC_EINVAL;
	}
}
