/* syscall.c - SVC dispatch. Tiap thread pakai ipc_space milik task-nya. */
#include "syscall.h"
#include "sched.h"

static struct task *kern_task;

void syscall_init(struct task *kt)
{
    kern_task = kt;
}

static struct ipc_space *dispatch_space(void)
{
    struct task *t = sched_current_task();
    if (t)
        return &t->ipc;
    /* Sebelum scheduler jalan (selftest single-threaded). */
    return kern_task ? &kern_task->ipc : 0;
}

void svc_dispatch(struct trap_regs *regs)
{
    unsigned num;
    struct ipc_space *sp;
    int ret = -2;   /* -2 = not a known syscall, leave r0 alone */

    if (!regs)
        return;
    sp = dispatch_space();
    if (!sp)
        return;
    num = regs->r[7];
    if (num == SYS_SEND) {
        ret = ipc_send(sp, regs->r[0],
                       (const struct ipc_wire *)regs->r[1], regs->r[2]);
    } else if (num == SYS_RECV) {
        ret = ipc_recv(sp, regs->r[0],
                       (struct ipc_wire *)regs->r[1], regs->r[2]);
    } else if (num == SYS_RPC) {
        ret = ipc_rpc(sp, regs->r[0],
                      (const struct ipc_wire *)regs->r[1], regs->r[2],
                      regs->r[3],
                      (struct ipc_wire *)regs->r[4], regs->r[5]);
    }
    if (ret != -2)
        regs->r[0] = (uint32_t)ret;
}
