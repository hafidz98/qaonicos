/*
 * ipc.h - Mach-style IPC (bring-up subset).
 *
 * A port is a kernel-protected queue of messages. Threads (of one task)
 * share an ipc_space: a table mapping small integer names to ports with
 * send/receive rights. mach_msg-style traps copy a flat wire message in
 * and out; the kernel queues zone-allocated copies.
 *
 * Non-blocking at this stage: send fails when the queue is full, receive
 * fails when it is empty.
 */
#ifndef _IPC_H_
#define _IPC_H_

#include <stdint.h>
#include "zone.h"

#define IPC_MSG_DATA    224
#define IPC_QDEPTH      8
#define IPC_NPORTS      32

#define IPC_SEND        1u
#define IPC_RECV        2u

/* Flat wire format exchanged with userspace (the trap ABI). */
struct ipc_wire {
    uint32_t bits;
    uint32_t size;              /* bytes of data[] used */
    uint32_t id;
    uint8_t  data[IPC_MSG_DATA];
};

/* Kernel message: queued copy. */
struct ipc_msg {
    struct ipc_msg *next;
    uint32_t bits;
    uint32_t size;
    uint32_t id;
    uint8_t  data[IPC_MSG_DATA];
};

struct ipc_port {
    struct ipc_msg *head, *tail;
    unsigned qlen;
    unsigned refs;
};

struct ipc_space {
    struct ipc_port *ports[IPC_NPORTS];
    unsigned rights[IPC_NPORTS];
    struct zone *port_zone;
    struct zone *msg_zone;
};

void ipc_space_init(struct ipc_space *sp, struct zone *port_zone,
                    struct zone *msg_zone);

/* Allocate a port with the given rights; returns its name (>0), 0 on failure. */
unsigned ipc_port_alloc(struct ipc_space *sp, unsigned rights);

/* Copy a wire message into the port's queue. 0 = ok, -1 = no right/full/bad. */
int ipc_send(struct ipc_space *sp, unsigned name,
             const struct ipc_wire *uwire, unsigned ulen);

/* Dequeue one message into uwire. Returns data size, -1 = no right/empty. */
int ipc_recv(struct ipc_space *sp, unsigned name,
             struct ipc_wire *uwire, unsigned ulen);

#endif /* _IPC_H_ */
