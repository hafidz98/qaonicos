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

/* Fase 8: syscall user mode. Pointer user divalidasi terhadap USER
 * range sebelum dipakai kernel (lihat user_range_ok). SYS_SEND/RECV/RPC
 * (primitif mentah, tanpa batasan ukuran) DITOLAK dari USR; dari user
 * hanya boleh lewat SYS_RPC_USER (bounce buffer kernel). */
#define SYS_WRITE    20u   /* r0=fd(1/2) r1=buf r2=len -> byte tertulis */
#define SYS_YIELD    21u   /* serahkan sisa slice, tunggu 1 tick */
#define SYS_EXIT     22u   /* akhiri thread pemanggil (tidak kembali) */
#define SYS_RPC_USER 23u   /* r0=sname r1=req r2=rlen r3=rname r4=rep r5=plen */
#define SYS_SBRK     24u   /* r0=inkremen byte -> brk lama (-1 gagal) */

/* Fase 9: syscall file ramfs (user only, seperti SYS_WRITE).
 * Nomor DISALIN MANUAL ke user/usys.h. */
#define SYS_OPEN     30u   /* r0=path_va r1=flags -> fd / -1 */
#define SYS_READ     31u   /* r0=fd r1=buf_va r2=len -> byte dibaca / -1 */
#define SYS_CLOSE    32u   /* r0=fd -> 0 / -1 */
#define SYS_LS       33u   /* r0=buf_va r1=max -> jumlah file */
#define SYS_DELETE   34u   /* r0=path_va -> 0 / -1 */

/* Fase 14: GPIO (user only, seperti SYS_WRITE). Bank di-fix 0 di
 * syscall; driver mendukung multi-bank untuk RV1103.
 * Nomor DISALIN MANUAL ke user/usys.h. */
#define SYS_GPIO_SET 40u   /* r0=pin r1=val(0/1) -> 0 / -1 */
#define SYS_GPIO_GET 41u   /* r0=pin -> 0/1 / -1 */

/* Fase 15: kartu SD (user only, seperti SYS_GPIO_*).
 * Nomor DISALIN MANUAL ke user/usys.h. */
#define SYS_SD_READ  50u   /* r0=sector(u32) r1=buf_va(512B) -> 0 / -1 */
#define SYS_SD_WRITE 51u   /* r0=sector(u32) r1=buf_va(512B) -> 0 / -1 */

void syscall_init(struct task *kern_task);
void svc_dispatch(struct trap_regs *regs);

#endif /* _SYSCALL_H_ */
