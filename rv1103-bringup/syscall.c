/* syscall.c - SVC dispatch. Threads of one task share kern_space. */
#include "syscall.h"

static struct ipc_space *kern_space;

void syscall_init(struct ipc_space *sp)
{
    kern_space = sp;
}

void svc_dispatch(struct trap_regs *regs)
{
    unsigned num;
    int ret = -2;   /* -2 = not a known syscall, leave r0 alone */

    if (!regs || !kern_space)
        return;
    num = regs->r[7];
    if (num == SYS_SEND) {
        ret = ipc_send(kern_space, regs->r[0],
                       (const struct ipc_wire *)regs->r[1], regs->r[2]);
    } else if (num == SYS_RECV) {
        ret = ipc_recv(kern_space, regs->r[0],
                       (struct ipc_wire *)regs->r[1], regs->r[2]);
    }
    if (ret != -2)
        regs->r[0] = (uint32_t)ret;
}
