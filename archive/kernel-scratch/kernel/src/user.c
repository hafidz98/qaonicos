/*
 * user.c - utilitas user mode: validasi range VA, user_kill, UART polled.
 *
 * user_kill() dipanggil dari arm_trap (trap.c) saat exception
 * (data/prefetch/undef abort) datang dari USR mode dan tidak bisa
 * diselesaikan pager. Ia TIDAK kembali ke stub: frame exception yang
 * faulting mungkin rusak, dan stub abort memarkir CPU dengan wfi/IRQ
 * ter-mask (hang). Sebagai gantinya: tandai THREAD_DEAD, hidupkan IRQ,
 * spin; tick berikutnya scheduler memindahkan CPU ke thread lain.
 *
 * C99, -ffreestanding, no libc.
 */
#include "user.h"
#include "trap.h"
#include "sched.h"
#include "board.h"

volatile unsigned user_done = 0u;

/* ------------------------------------------------------------------ */
/* Validasi range VA user                                             */
/* ------------------------------------------------------------------ */
int user_range_ok(uint32_t va, uint32_t len)
{
    if (len == 0u)
        return 1;
    if (va < USER_VA_BASE || va >= USER_VA_END)
        return 0;
    if (len > USER_VA_END - va)   /* menolak len yang melampaui END / wrap */
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* UART polled (duplikat minimal ala trap.c: uart.c me-hardcode       */
/* RV1103, jadi bawa sendiri untuk QEMU virt PL011).                   */
/* ------------------------------------------------------------------ */
#if BOARD_RV1103
#define U_UART_DR   0xff4c0000u
#define U_UART_FR   0xff4c0018u
#define U_FR_TXFF   (1u << 5)
#else
#define U_UART_DR   0x09000000u
#define U_UART_FR   0x09000018u
#define U_FR_TXFF   (1u << 5)
#endif

void uputc(char c)
{
    volatile uint32_t *dr = (volatile uint32_t *)U_UART_DR;
    volatile uint32_t *fr = (volatile uint32_t *)U_UART_FR;
    while ((*fr & U_FR_TXFF) != 0u) { }
    *dr = (uint32_t)(unsigned char)c;
    if (c == '\n') {
        while ((*fr & U_FR_TXFF) != 0u) { }
        *dr = (uint32_t)'\r';
    }
}

void uputs(const char *s)
{
    while (s && *s)
        uputc(*s++);
}

/* Fase 17: baca 1 byte console non-blocking (polled UART).
 * 0-255 bila ada byte menunggu, -1 bila FIFO kosong. Dipakai
 * SYS_READ_CONSOLE (tombol 'q' untuk keluar dari umon live).
 * PL011 (virt): FR bit 4 = RXFE. DW APB (RV1103): LSR bit 0 = DR. */
int console_getc_nb(void)
{
#if BOARD_RV1103
    volatile uint32_t *rbr = (volatile uint32_t *)0xff4c0000u;
    volatile uint32_t *lsr = (volatile uint32_t *)0xff4c0014u;
    if ((*lsr & 1u) == 0u)
        return -1;
    return (int)(*rbr & 0xFFu);
#else
    volatile uint32_t *dr = (volatile uint32_t *)U_UART_DR;
    volatile uint32_t *fr = (volatile uint32_t *)U_UART_FR;
    if ((*fr & (1u << 4)) != 0u)   /* RXFE: FIFO kosong */
        return -1;
    return (int)(*dr & 0xFFu);
#endif
}

static void uputx(uint32_t v)
{
    static const char h[] = "0123456789abcdef";
    int i;
    uputs("0x");
    for (i = 7; i >= 0; i--)
        uputc(h[(v >> (i * 4)) & 0xFu]);
}

/* ------------------------------------------------------------------ */
/* user_kill - akhiri thread user yang fault. Tulis ulang frame agar  */
/* stub abort rfefd ke user_exit_trampoline, lalu KEMBALI.           */
/* ------------------------------------------------------------------ */
void user_kill(struct trap_regs *regs, const char *kind,
               uint32_t fsr, uint32_t far)
{
    struct sched_thread *t = sched_current_thread();
    int i;

    uputs("\n[user] FAULT (");
    uputs(kind);
    uputs(") dari USR mode - thread dibunuh\n");
    uputs("  fsr=");
    uputx(fsr);
    uputs(" far=");
    uputx(far);
    uputs("\n");
    if (regs) {
        uputs("  r0-r3=");
        for (i = 0; i < 4; i++) {
            uputx(regs->r[i]);
            uputc(' ');
        }
        uputs(" pc~=");
        uputx(regs->lr - 8u);   /* lr frame = alamat fault+8 (ala trap.c) */
        uputs(" spsr=");
        uputx(regs->spsr);
        uputs("\n");
    }
    if (t)
        t->state = THREAD_DEAD;

    /* Tandai selesai agar gerbang halt di report() tidak menunggu
     * selamanya; UART log menunjukkan tidak ada "USER TESTS PASSED"
     * bila thread mati sebelum tes selesai. */
    user_done = 1u;

    /* Tulis ulang frame: stub abort akan rfefd ke trampolin (SVC,
     * IRQ hidup) bukan ke instruksi yang fault. Lalu KEMBALI.
     * (Tidak boleh spin di sini: stub harus mem-pop frame-nya;
     * abort bersarang - mis. pager yang sedang block - mengandalkan
     * keseimbangan stack ABT.) */
    if (regs) {
        regs->lr = (uint32_t)user_exit_trampoline;
        regs->spsr = 0x13u;     /* SVC, IRQ on */
    }
    /* kembali; stub rfefd ke trampolin */
}

/* user_exit_trampoline - tujuan rfefd stub abort pasca user_kill.    */
/* Berjalan di SVC mode. Tandai DEAD (aman diulang), hidupkan IRQ,   */
/* spin sampai tick menjadwalkan thread lain.                        */
/* ------------------------------------------------------------------ */
void user_exit_trampoline(void)
{
    struct sched_thread *t = sched_current_thread();
    if (t)
        t->state = THREAD_DEAD;
    user_done = 1u;
    __asm__ volatile("cpsie i" ::: "memory");
    for (;;) { }
    /* tak tercapai */
}
