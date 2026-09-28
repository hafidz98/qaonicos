/*
 * usys.h - ABI userspace: nomor syscall + format wire IPC.
 *
 * Nomor syscall DISALIN MANUAL dari rv1103-bringup/syscall.h
 * (program user tidak include header kernel). Bila nomor di kernel
 * berubah, file ini harus ikut diubah.
 */
#ifndef _USYS_H_
#define _USYS_H_

#define SYS_WRITE    20u
#define SYS_YIELD    21u
#define SYS_EXIT     22u
#define SYS_RPC_USER 23u
#define SYS_SBRK     24u

/* Fase 9: syscall file ramfs (DISALIN MANUAL dari syscall.h kernel). */
#define SYS_OPEN     30u
#define SYS_READ     31u
#define SYS_CLOSE    32u
#define SYS_LS       33u
#define SYS_DELETE   34u

/* Fase 14: GPIO (DISALIN MANUAL dari syscall.h kernel). */
#define SYS_GPIO_SET 40u
#define SYS_GPIO_GET 41u

/* Fase 15: kartu SD (DISALIN MANUAL dari syscall.h kernel). */
#define SYS_SD_READ  50u
#define SYS_SD_WRITE 51u

#define O_RDONLY 0u
#define O_WRONLY 1u
#define O_RDWR   2u
#define O_CREAT  0x40u

/* Port well-known (disepakati dengan kernel, user.h): */
#define U_SEND_PORT  1u      /* send-right ke echo server */
#define U_REPLY_PORT 2u      /* recv port untuk reply */

/* ID pesan echo (disepakati dengan kernel, user.h): */
#define ECHO_REQ_ID  0x8001u
#define ECHO_REP_ID  0x8002u

/* Format wire IPC: SAMA PERSIS dengan struct ipc_wire kernel
 * (bits, size, id + 4128 byte data). Jangan diubah tanpa mengubah
 * ipc.h kernel. */
#define U_WIRE_DATA 4128u

struct user_wire {
    unsigned bits;
    unsigned size;   /* byte data valid (termasuk NUL bila string) */
    unsigned id;
    unsigned char data[U_WIRE_DATA];
};

#endif /* _USYS_H_ */
