/* task.c - konstruksi protection domain. */
#include "task.h"

static unsigned next_task_id = 1u;

void task_create(struct task *t, struct zone *port_zone,
                 struct zone *msg_zone)
{
    unsigned i;

    if (!t)
        return;
    t->id = next_task_id++;
    t->refs = 1u;
    t->brk = 0u;                /* diisi SYS_SBRK / boot task user */
    /* Fase 9: fd table per-task; 0/1/2 = console (dipakai), sisanya
     * bebas. */
    for (i = 0u; i < FS_MAX_FD; i++) {
        t->fds[i].used = 0;
        t->fds[i].fidx = FS_FD_CONSOLE;
        t->fds[i].gen = 0u;
        t->fds[i].off = 0u;
        t->fds[i].flags = 0u;
    }
    t->fds[0].used = 1;
    t->fds[1].used = 1;
    t->fds[2].used = 1;
    /* Pool L1 cukup untuk bring-up (4 slot, dipakai 3: kern, A, B).
     * vm_space_init menyalin tabel kernel sehingga pemetaan 1:1 DRAM
     * + device tetap identik di semua task. */
    vm_space_init(&t->vm);
    ipc_space_init(&t->ipc, port_zone, msg_zone);
}

unsigned task_id(const struct task *t)
{
    return t ? t->id : 0u;
}
