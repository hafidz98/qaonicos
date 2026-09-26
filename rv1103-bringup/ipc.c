/* ipc.c - ports, spaces, message queues. */
#include "ipc.h"
#include "lib.h"

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
    sp->ports[i] = p;
    sp->rights[i] = rights;
    return i + 1;
}

int ipc_send(struct ipc_space *sp, unsigned name,
             const struct ipc_wire *uwire, unsigned ulen)
{
    struct ipc_port *port;
    struct ipc_msg *m;

    if (ulen < 12 || ulen > sizeof(struct ipc_wire))
        return -1;
    port = port_lookup(sp, name, IPC_SEND);
    if (!port || port->qlen >= IPC_QDEPTH)
        return -1;
    m = (struct ipc_msg *)zalloc(sp->msg_zone);
    if (!m)
        return -1;
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
    return 0;
}

int ipc_recv(struct ipc_space *sp, unsigned name,
             struct ipc_wire *uwire, unsigned ulen)
{
    struct ipc_port *port;
    struct ipc_msg *m;
    int size;

    if (ulen < sizeof(struct ipc_wire))
        return -1;
    port = port_lookup(sp, name, IPC_RECV);
    if (!port || !port->head)
        return -1;
    m = port->head;
    port->head = m->next;
    if (!port->head)
        port->tail = 0;
    port->qlen--;

    uwire->bits = m->bits;
    uwire->id = m->id;
    uwire->size = m->size;
    memcpy(uwire->data, m->data, m->size);
    size = (int)m->size;
    zfree(sp->msg_zone, m);
    return size;
}
