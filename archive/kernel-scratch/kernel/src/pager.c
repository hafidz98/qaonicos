/* pager.c - memory object + protokol pager eksternal via IPC.
 *
 * Bring-up ala Mach: data_request/data_supply berjalan di atas
 * ipc_rpc yang sudah ada (Fase 6). Pager adalah thread biasa di task
 * lain; kernel bertindak sebagai kliennya dari dalam fault handler.
 */
#include "pager.h"
#include "task.h"
#include "ipc.h"
#include "sched.h"
#include "vm.h"
#include "lib.h"

#define VM_NOBJ 8u
#define VM_NMAP 8u

static struct vm_object objs[VM_NOBJ];
static unsigned obj_next = 1u;

static struct {
    struct task *task;
    uint32_t va;             /* base jendela, sejajar halaman */
    unsigned npages;
    struct vm_object *obj;
    uint32_t obj_off;
    unsigned prot;
} maps[VM_NMAP];

/* Buffer wire statis: lihat penjelasan di pager.h. */
static struct ipc_wire rq_w, rp_w;

unsigned vm_object_create(struct task *owner, struct task *pager_task,
                          unsigned req_name, unsigned rep_name,
                          uint32_t size)
{
    int i;

    if (!owner || !pager_task || req_name == 0 || rep_name == 0 ||
        size == 0)
        return 0;
    for (i = 0; i < (int)VM_NOBJ; i++)
        if (objs[i].id == 0)
            break;
    if (i == (int)VM_NOBJ)
        return 0;
    objs[i].id = obj_next++;
    objs[i].owner = owner;
    objs[i].pager_task = pager_task;
    objs[i].req_name = req_name;
    objs[i].rep_name = rep_name;
    objs[i].size = size;
    return objs[i].id;
}

int vm_object_destroy(unsigned obj_id)
{
    int i, j;

    for (i = 0; i < (int)VM_NOBJ; i++)
        if (objs[i].id == obj_id)
            break;
    if (i == (int)VM_NOBJ)
        return -1;
    for (j = 0; j < (int)VM_NMAP; j++)
        if (maps[j].task && maps[j].obj == &objs[i])
            maps[j].task = 0;
    objs[i].id = 0;
    return 0;
}

int vm_map_object(struct task *t, uint32_t va, unsigned obj_id,
                  uint32_t obj_offset, unsigned npages, unsigned prot)
{
    struct vm_object *o = 0;
    int i, mi = -1;

    if (!t || (va & VM_PAGE_MASK) || npages == 0)
        return -1;
    if (va < VM_DEMAND_BASE ||
        va + (uint32_t)npages * VM_PAGE_SIZE > VM_DEMAND_END)
        return -1;
    for (i = 0; i < (int)VM_NOBJ; i++)
        if (objs[i].id == obj_id) {
            o = &objs[i];
            break;
        }
    if (!o)
        return -1;
    if (obj_offset + (uint32_t)npages * VM_PAGE_SIZE > o->size)
        return -1;
    for (i = 0; i < (int)VM_NMAP; i++)
        if (maps[i].task == 0) {
            mi = i;
            break;
        }
    if (mi < 0)
        return -1;
    maps[mi].task = t;
    maps[mi].va = va;
    maps[mi].npages = npages;
    maps[mi].obj = o;
    maps[mi].obj_off = obj_offset;
    maps[mi].prot = prot;
    return 0;
}

int pager_lookup(struct task *t, uint32_t va, struct vm_object **o,
                 unsigned *obj_id, uint32_t *off, unsigned *prot)
{
    int i;
    uint32_t vend;

    if (!t || (va & VM_PAGE_MASK))
        return 0;
    for (i = 0; i < (int)VM_NMAP; i++) {
        if (maps[i].task != t)
            continue;
        vend = maps[i].va + (uint32_t)maps[i].npages * VM_PAGE_SIZE;
        if (va >= maps[i].va && va < vend) {
            *o = maps[i].obj;
            *obj_id = maps[i].obj->id;
            *off = maps[i].obj_off + (va - maps[i].va);
            *prot = maps[i].prot;
            return 1;
        }
    }
    return 0;
}

int pager_fetch_page(struct vm_object *o, unsigned obj_id, uint32_t off,
                     uint8_t *buf)
{
    int n;
    uint32_t rid, roff;

    if (!o || !buf)
        return -1;
    rq_w.bits = 0;
    rq_w.id = MSG_DATA_REQUEST;
    *(uint32_t *)rq_w.data = obj_id;
    *(uint32_t *)(rq_w.data + 4) = off;
    rq_w.size = 8;

    /* Blocking RPC. Aman dari abort handler: ipc_recv Fase 6
     * meng-enable IRQ selama spin (cpsie i) lalu me-restore mask,
     * sehingga tick timer tetap menjadwalkan pager thread. */
    n = ipc_rpc(&o->pager_task->ipc, o->req_name,
                &rq_w, sizeof(rq_w), o->rep_name, &rp_w, sizeof(rp_w));
    if (n < 0)
        return -1;
    if (rp_w.id != MSG_DATA_SUPPLY)
        return -1;
    if ((uint32_t)n < 8u + VM_PAGE_SIZE)
        return -1;
    rid = *(uint32_t *)rp_w.data;
    roff = *(uint32_t *)(rp_w.data + 4);
    if (rid != obj_id || roff != off)
        return -1;
    memcpy(buf, rp_w.data + 8, VM_PAGE_SIZE);
    return 0;
}
