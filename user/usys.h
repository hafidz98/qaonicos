/*
 * usys.h - ABI userspace: nomor syscall + format wire IPC.
 *
 * Nomor syscall DISALIN MANUAL dari kernel/src/syscall.h
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

/* Fase 16: filesystem FAT32 di /sd (DISALIN MANUAL dari syscall.h). */
#define SYS_MKDIR      52u
#define SYS_FAT_WRITE  53u
#define SYS_FAT_READ   54u
#define SYS_FAT_DELETE 55u
#define SYS_READDIR    56u

/* Fase 17: TUI / umon (DISALIN MANUAL dari syscall.h kernel). */
#define SYS_STAT         57u
#define SYS_TLIST        58u
#define SYS_READ_CONSOLE 59u

/* Statistik sistem (Fase 17, DISALIN MANUAL dari syscall.h).
 * Semua field unsigned 32-bit, sizeof = 36. */
struct qaon_stat {
    unsigned uptime_ms;
    unsigned cpu_pct;
    unsigned mem_used_kb;
    unsigned mem_total_kb;
    unsigned blk_total_sec;
    unsigned blk_used_sec;
    unsigned net_rx_kb;
    unsigned net_tx_kb;
    unsigned nthreads;
};

/* Satu baris daftar thread (Fase 17, DISALIN MANUAL dari syscall.h).
 * state: 0=RUNNABLE, 1=BLOCKED, 2=DEAD. user: 1=user, 0=kernel.
 * sizeof = 12. */
struct qaon_tentry {
    unsigned id;
    unsigned state;
    unsigned user;
};

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
