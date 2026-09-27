/*
 * syscall.h - SVC dispatch to kernel services (bring-up).
 *
 * r7 = syscall number (Mach-style; real Mach uses a trap number too).
 *   10: sys_send  r0 = port name, r1 = wire ptr, r2 = wire len -> 0 / -1
 *   11: sys_recv  r0 = port name, r1 = wire buf, r2 = buf len  -> size / -1
 *                 (blocking once the scheduler runs)
 *   12: sys_rpc   r0 = send name, r1 = req ptr, r2 = req len,
 *                 r3 = reply name, r4 = rep buf, r5 = rep len -> size / -1
 *                 (send request, then blocking receive of the reply)
 * Return value is written back to the trapped r0.
 *
 * Fase 6: dispatch memakai ipc_space milik task thread pemanggil;
 * sebelum scheduler jalan (selftest), dipakai space task kernel.
 */
#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "trap.h"
#include "task.h"

#define SYS_SEND    10u
#define SYS_RECV    11u
#define SYS_RPC     12u

void syscall_init(struct task *kern_task);
void svc_dispatch(struct trap_regs *regs);

#endif /* _SYSCALL_H_ */
