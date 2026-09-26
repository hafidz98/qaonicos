/*
 * syscall.h - SVC dispatch to kernel services (bring-up).
 *
 * r7 = syscall number (Mach-style; real Mach uses a trap number too).
 *   10: sys_send  r0 = port name, r1 = wire ptr, r2 = wire len -> 0 / -1
 *   11: sys_recv  r0 = port name, r1 = wire buf, r2 = buf len  -> size / -1
 * Return value is written back to the trapped r0.
 */
#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "trap.h"
#include "ipc.h"

#define SYS_SEND    10u
#define SYS_RECV    11u

void syscall_init(struct ipc_space *sp);
void svc_dispatch(struct trap_regs *regs);

#endif /* _SYSCALL_H_ */
