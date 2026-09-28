/*
 * ipc.h - Mach-style IPC (bring-up subset).
 *
 * A port is a kernel-protected queue of messages. Threads of one task
 * share an ipc_space: a table mapping small integer names to ports with
 * send/receive rights. mach_msg-style traps copy a flat wire message in
 * and out; the kernel queues zone-allocated copies.
 *
 * Fase 6: receive is BLOCKING once the scheduler runs (the receiver's
 * thread is marked BLOCKED and woken by the sender); before the
 * scheduler starts, recv stays non-blocking so single-threaded
 * self-tests keep working. Send stays non-blocking (fails when the
 * queue is full). Ports can be granted into another task's namespace
 * with independent rights (the Mach way of handing out send rights).
 */
#ifndef _IPC_H_
#define _IPC_H_

#include <stdint.h>
#include "zone.h"

/* Fase 7: IPC_MSG_DATA 224 -> 4128. Protokol pager butuh satu halaman
 * penuh (4096 byte) + header {obj_id, offset} (8 byte) = 4104 byte
 * dalam satu pesan. msg_pool di kernel_main.c berukuran
 * 16*sizeof(struct ipc_msg) sehingga ikut membesar otomatis (+-66KB
 * BSS). Harga: struct ipc_wire kini ~4KB - jangan taruh dua wire di
 * stack 8KB; thread stack dibesarkan ke 16KB dan jalur abort pakai
 * buffer statis (stack abort cuma 8KB). */
#define IPC_MSG_DATA    4128
#define IPC_QDEPTH      8
#define IPC_NPORTS      32

#define IPC_SEND        1u
#define IPC_RECV        2u

/* Forward: waiter tanpa siklus include (sched.h -> task.h -> ipc.h). */
struct sched_thread;

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
    struct sched_thread *waiter;  /* blocking receiver, 0 bila kosong */
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

/* Dequeue one message into uwire. Returns data size, -1 = no right/bad.
 * Blocking when the queue is empty and the scheduler runs; -1 when the
 * scheduler is not running yet (single-threaded self-test). */
int ipc_recv(struct ipc_space *sp, unsigned name,
             struct ipc_wire *uwire, unsigned ulen);

/* Grant: sisipkan port milik src/name ke namespace dst dengan rights
 * independen (port-nya berbagi, refs++). Cara server memberi
 * send-right ke client. Mengembalikan nama di dst, 0 bila gagal. */
unsigned ipc_port_grant(struct ipc_space *dst, struct ipc_space *src,
                        unsigned name, unsigned rights);

/* Lepas nama dari namespace; bebaskan port bila refs habis. */
int ipc_port_dealloc(struct ipc_space *sp, unsigned name);

/* RPC sinkron: ipc_send request lalu blocking ipc_recv balasan. */
int ipc_rpc(struct ipc_space *sp, unsigned send_name,
            const struct ipc_wire *req, unsigned reqlen,
            unsigned reply_name, struct ipc_wire *rep, unsigned replen);

#endif /* _IPC_H_ */
