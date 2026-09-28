/*
 * user.h - Konstanta user mode + syscall (Fase 8, bring-up).
 *
 * Layout VA user (di dalam demand range 0x10000000-0x11000000, jadi
 * pager tetap bisa melayani fault di sini):
 *   0x10030000 (4 halaman) : program userspace (R+X, di-embed dari
 *                            user/hello.bin saat boot)
 *   0x1003E000-0x10040000 : stack user (2 halaman RW, XN)
 *   0x10050000 ...        : program break (sbrk, tumbuh ke atas)
 */
#ifndef _USER_H_
#define _USER_H_

#include <stdint.h>

/* Batas VA yang boleh disentuh dari user mode (== demand range). */
#define USER_VA_BASE    0x10000000u
#define USER_VA_END     0x11000000u

#define USER_PROG_VA    0x10030000u
#define USER_PROG_PAGES 4u

#define USER_STACK_TOP   0x10040000u
#define USER_STACK_PAGES 2u

#define USER_BRK_START  0x10050000u

/* Fase 9: program uji filesystem (user/fstest.c). Di dalam demand
 * range, tidak tabrakan dengan PAGER_VA (0x10010000, task_c),
 * COW_VA (0x10020000, task_a/b), program hello (0x10030000),
 * stack hello (0x1003E000) maupun brk (0x10050000). */
#define FSTEST_PROG_VA     0x10028000u
#define FSTEST_PROG_PAGES  4u
#define FSTEST_STACK_TOP   0x1002E000u
#define FSTEST_STACK_PAGES 2u

/* Fase 10: init + utilitas userspace (user/init.c, ucat.c, uls.c,
 * uecho.c). Semua di dalam demand range, tidak tabrakan dengan
 * region yang sudah ada:
 *   0x10012000 init        (4 halaman R+X)
 *   0x10014000 stack init  (2 halaman RW, top 0x10016000)
 *   0x10018000 ucat        (4 halaman R+X)
 *   0x1001C000 stack ucat  (2 halaman RW, top 0x1001E000)
 *   0x10021000 uls         (4 halaman R+X)
 *   0x1001E000 stack uls   (2 halaman RW, top 0x10020000;
 *                           COW_VA 0x10020000 ada di vm task_a/task_b,
 *                           bukan task_user)
 *   0x10040000 uecho       (4 halaman R+X; tepat di atas stack hello
 *                           yang berakhir di 0x10040000)
 *   0x10044000 stack uecho (2 halaman RW, top 0x10046000) */
#define INIT_PROG_VA     0x10012000u
#define INIT_PROG_PAGES  4u
#define INIT_STACK_TOP   0x10016000u
#define INIT_STACK_PAGES 2u

#define UCAT_PROG_VA     0x10018000u
#define UCAT_PROG_PAGES  4u
#define UCAT_STACK_TOP   0x1001E000u
#define UCAT_STACK_PAGES 2u

#define ULS_PROG_VA      0x10021000u
#define ULS_PROG_PAGES   4u
#define ULS_STACK_TOP    0x10020000u
#define ULS_STACK_PAGES  2u

#define UECHO_PROG_VA    0x10040000u
#define UECHO_PROG_PAGES 4u
#define UECHO_STACK_TOP  0x10046000u
#define UECHO_STACK_PAGES 2u

/* Fase 14: utilitas GPIO userspace (user/ugpio.c). Di dalam demand
 * range, tidak tabrakan dengan region yang sudah ada:
 *   0x10048000 ugpio       (4 halaman R+X; di atas stack uecho yang
 *                           berakhir di 0x10046000)
 *   0x1004C000 stack ugpio (2 halaman RW, top 0x1004E000; di bawah
 *                           USER_BRK_START 0x10050000) */
#define UGPIO_PROG_VA     0x10048000u
#define UGPIO_PROG_PAGES  4u
#define UGPIO_STACK_TOP   0x1004E000u
#define UGPIO_STACK_PAGES 2u

/* Nama port well-known di task_user.ipc (diisi kernel saat boot). */
#define USER_SVC_SEND   1u      /* send-right ke echo server */
#define USER_SVC_REPLY  2u      /* recv port untuk reply */

/* ID pesan echo (disepakati dengan user/hello.c via user/usys.h). */
#define ECHO_REQ_ID     0x8001u
#define ECHO_REP_ID     0x8002u

struct trap_regs;

/* 1 = [va, va+len) seluruhnya di dalam USER range (tanpa wrap). */
int user_range_ok(uint32_t va, uint32_t len);

/* Bunuh thread user yang fault: cetak forensik, tandai THREAD_DEAD,
 * lalu TULIS ULANG frame (regs->lr -> user_exit_trampoline,
 * regs->spsr -> SVC) dan KEMBALI. Stub abort akan rfefd ke trampolin.
 *
 * PENTING: tidak boleh spin di sini! Stub harus mem-pop frame-nya;
 * abort bersarang (mis. pager yang sedang block di bawah frame ini)
 * mengandalkan keseimbangan stack ABT. Spin hanya di trampolin
 * (SVC mode) setelah frame di-pop. */
void user_kill(struct trap_regs *regs, const char *kind,
               uint32_t fsr, uint32_t far);

/* Trampolin exit user: tujuan rfefd stub abort (berjalan di SVC mode).
 * Menandai thread DEAD (lagi, aman), set user_done, hidupkan IRQ,
 * spin sampai tick menjadwalkan thread lain. */
void user_exit_trampoline(void);

/* Diset saat thread user selesai (exit normal atau dibunuh karena
 * fault). Dipakai report() sebagai gerbang halt. */
extern volatile unsigned user_done;

/* UART polled minimal untuk pesan user-mode (ala trap.c abt_putc). */
void uputc(char c);
void uputs(const char *s);

#endif /* _USER_H_ */
