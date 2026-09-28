/* ipc.c - ports, spaces, message queues, blocking IPC. */
#include "ipc.h"
#include "sched.h"
#include "lib.h"

/* IRQ mask/unmask untuk seksi kritis pendek (cek-antrean + daftar
 * waiter harus atomik terhadap tick timer, kalau tidak wakeup bisa
 * terlewat -> deadlock). */
static inline unsigned ipc_irq_save(void)
{
    unsigned cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) :: "memory");
    return cpsr;
}

static inline void ipc_irq_restore(unsigned cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
}

void ipc_space_init(struct ipc_space *sp, struct zone *port_zone,
                    struct zone *msg_zone)
{
    unsigned i;
    for (i = 0; i < IPC_NPORTS; i++) {
        sp->ports[i] = 0;
        sp->rights[i] = 0;
    }
    sp->port_zone = port_zone;
    sp->msg_zone = msg_zone;
}

static struct ipc_port *port_lookup(struct ipc_space *sp, unsigned name,
                                    unsigned need)
{
    unsigned idx;
    if (name == 0 || name > IPC_NPORTS)
        return 0;
    idx = name - 1;
    if (!sp->ports[idx] || (sp->rights[idx] & need) != need)
        return 0;
    return sp->ports[idx];
}

unsigned ipc_port_alloc(struct ipc_space *sp, unsigned rights)
{
    unsigned i;
    struct ipc_port *p;

    for (i = 0; i < IPC_NPORTS; i++)
        if (!sp->ports[i])
            break;
    if (i == IPC_NPORTS)
        return 0;
    p = (struct ipc_port *)zalloc(sp->port_zone);
    if (!p)
        return 0;
    p->head = p->tail = 0;
    p->qlen = 0;
    p->refs = 1;
    p->waiter = 0;
    sp->ports[i] = p;
    sp->rights[i] = rights;
    return i + 1;
}

int ipc_send(struct ipc_space *sp, unsigned name,
             const struct ipc_wire *uwire, unsigned ulen)
{
    struct ipc_port *port;
    struct ipc_msg *m;
    unsigned s;
    int rc = -1;

    if (ulen < 12 || ulen > sizeof(struct ipc_wire))
        return -1;
    s = ipc_irq_save();
    port = port_lookup(sp, name, IPC_SEND);
    if (!port || port->qlen >= IPC_QDEPTH)
        goto out;
    m = (struct ipc_msg *)zalloc(sp->msg_zone);
    if (!m)
        goto out;
    m->next = 0;
    m->bits = uwire->bits;
    m->id = uwire->id;
    m->size = uwire->size;
    if (m->size > IPC_MSG_DATA)
        m->size = IPC_MSG_DATA;
    memcpy(m->data, uwire->data, m->size);

    if (port->tail)
        port->tail->next = m;
    else
        port->head = m;
    port->tail = m;
    port->qlen++;

    /* Bangunkan penerima yang sedang blocking-recv, bila ada. Wakeup
     * dilakukan SETELAH pesan masuk antrean: penerima yang bangun
     * pasti menemukan pesannya saat cek ulang. */
    if (port->waiter) {
        sched_wakeup(port->waiter);
        port->waiter = 0;
    }
    rc = 0;
out:
    ipc_irq_restore(s);
    return rc;
}

int ipc_recv(struct ipc_space *sp, unsigned name,
             struct ipc_wire *uwire, unsigned ulen)
{
    struct ipc_port *port;
    struct ipc_msg *m;
    struct sched_thread *t;
    int size;
    unsigned s;

    if (ulen < sizeof(struct ipc_wire))
        return -1;
    for (;;) {
        s = ipc_irq_save();
        port = port_lookup(sp, name, IPC_RECV);
        if (port && port->head) {
            m = port->head;
            port->head = m->next;
            if (!port->head)
                port->tail = 0;
            port->qlen--;
            ipc_irq_restore(s);

            uwire->bits = m->bits;
            uwire->id = m->id;
            uwire->size = m->size;
            memcpy(uwire->data, m->data, m->size);
            size = (int)m->size;
            zfree(sp->msg_zone, m);
            return size;
        }
        /*
         * Antrean kosong (atau nama/rights salah). Sebelum scheduler
         * jalan tidak ada yang bisa membangunkan kita -> non-blocking.
         */
        t = sched_current_thread();
        if (!port || !t) {
            ipc_irq_restore(s);
            return -1;
        }
        /* Satu waiter per port cukup untuk bring-up; pemblokir kedua
         * ditolak daripada menggantung pemblokir pertama. */
        if (port->waiter) {
            ipc_irq_restore(s);
            return -1;
        }
        /* Daftarkan waiter + tandai BLOCKED dalam satu seksi kritis:
         * pengirim yang datang di antaranya pasti melihat waiter.
         *
         * PENTING: entry eksepsi SVC me-mask IRQ (I=1). Kalau spin dengan
         * IRQ masked, tick timer tidak pernah fire -> deadlock. Maka
         * eksplisit cpsie i sebelum spin; ipc_irq_restore(s) setelah
         * bangun mengembalikan state IRQ seperti saat masuk. */
        port->waiter = t;
        t->state = THREAD_BLOCKED;
        __asm__ volatile("cpsie i" ::: "memory");

        /* Tunggu dibangunkan. Tick preemptif memindahkan kita keluar;
         * kita lanjut tepat di sini setelah sched_wakeup() +
         * dijadwalkan lagi, lalu cek ulang antrean. */
        while (t->state == THREAD_BLOCKED) {
            /* IRQ hidup: tick bisa menyela kapan saja. */
        }
        ipc_irq_restore(s);
    }
}

unsigned ipc_port_grant(struct ipc_space *dst, struct ipc_space *src,
                        unsigned name, unsigned rights)
{
    struct ipc_port *p;
    unsigned i, s;

    if (!dst || !src || name == 0 || name > IPC_NPORTS)
        return 0;
    s = ipc_irq_save();
    p = src->ports[name - 1];
    if (!p) {
        ipc_irq_restore(s);
        return 0;
    }
    for (i = 0; i < IPC_NPORTS; i++)
        if (!dst->ports[i])
            break;
    if (i == IPC_NPORTS) {
        ipc_irq_restore(s);
        return 0;
    }
    dst->ports[i] = p;
    dst->rights[i] = rights;
    p->refs++;
    ipc_irq_restore(s);
    return i + 1;
}

int ipc_port_dealloc(struct ipc_space *sp, unsigned name)
{
    struct ipc_port *p;
    struct ipc_msg *m, *nx;
    unsigned idx, s;

    if (!sp || name == 0 || name > IPC_NPORTS)
        return -1;
    idx = name - 1;
    s = ipc_irq_save();
    p = sp->ports[idx];
    if (!p) {
        ipc_irq_restore(s);
        return -1;
    }
    sp->ports[idx] = 0;
    sp->rights[idx] = 0;
    if (p->refs > 0)
        p->refs--;
    if (p->refs == 0) {
        /* Jangan gantung thread yang sedang block di port ini:
         * bangunkan; ia akan melihat nama sudah mati -> -1. */
        if (p->waiter) {
            sched_wakeup(p->waiter);
            p->waiter = 0;
        }
        m = p->head;
        while (m) {
            nx = m->next;
            zfree(sp->msg_zone, m);
            m = nx;
        }
        zfree(sp->port_zone, p);
    }
    ipc_irq_restore(s);
    return 0;
}

int ipc_rpc(struct ipc_space *sp, unsigned send_name,
            const struct ipc_wire *req, unsigned reqlen,
            unsigned reply_name, struct ipc_wire *rep, unsigned replen)
{
    if (ipc_send(sp, send_name, req, reqlen) != 0)
        return -1;
    /* Blocking: server membalas ke reply port, balasan membangunkan. */
    return ipc_recv(sp, reply_name, rep, replen);
}
