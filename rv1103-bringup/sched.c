/*
 * sched.c - Preemptive round-robin scheduler.
 *
 * BUILD WITH -mgeneral-regs-only (see sched.h for why).
 */
#include "sched.h"
#include "gic.h"
#include "timer.h"
#include "vm.h"

static struct sched_thread threads[SCHED_MAX_THREADS];
static unsigned nthreads;
static unsigned cur;
static volatile unsigned ticks;
static uint32_t slice_ticks;

void sched_init(void)
{
    nthreads = 0;
    cur = 0;
    ticks = 0;
    slice_ticks = 0;
}

void sched_add(void (*entry)(void), uint8_t *stack_top,
               struct task *task)
{
    uint32_t *f;
    unsigned i;

    if (nthreads >= SCHED_MAX_THREADS || !entry || !stack_top)
        return;
    f = (uint32_t *)stack_top - FR_WORDS;
    f[0] = 0u;                          /* pad */
    for (i = 1; i <= 13; i++)
        f[i] = 0u;                      /* r0..r12 */
    f[FR_PC] = (uint32_t)entry;
    f[FR_CPSR] = 0x13u;                 /* SVC mode, IRQ enabled */

    threads[nthreads].sp = f;
    for (i = 0; i < 32; i++)
        threads[nthreads].vfp.d[i] = 0u;
    threads[nthreads].vfp.fpscr = 0u;
    threads[nthreads].id = (int)nthreads;
    threads[nthreads].task = task;
    threads[nthreads].state = THREAD_RUNNABLE;
    nthreads++;
}

void sched_set_slice(uint32_t ticks)
{
    slice_ticks = ticks;
}

unsigned sched_ticks(void)
{
    return ticks;
}

struct sched_thread *sched_current_thread(void)
{
    if (nthreads == 0)
        return 0;
    return &threads[cur];
}

struct task *sched_current_task(void)
{
    struct sched_thread *t = sched_current_thread();
    return t ? t->task : 0;
}

void sched_block_current(void)
{
    struct sched_thread *t = sched_current_thread();
    unsigned cpsr;

    if (!t)
        return;
    /*
     * Tandai BLOCKED secara atomik terhadap tick timer, lalu spin.
     *
     * Kenapa spin, bukan switch langsung dari sini: kita dipanggil
     * dari dalam SVC (di bawah frame SVC masih ada call chain C
     * arm_trap -> svc_dispatch -> ipc_recv), dan frame SVC
     * (r0..r12, pad, lr_svc, spsr_svc) TIDAK kompatibel dengan frame
     * 16-word IRQ ([0]=pad, [1..13]=r0..r12, [14]=pc, [15]=cpsr) yang
     * dipakai scheduler. Jadi serahkan perpindahan ke tick preemptif:
     * tick berikutnya melihat status BLOCKED dan memilih thread lain.
     * Pengirim membangunkan kita via sched_wakeup(); saat dijadwalkan
     * lagi, eksekusi lanjut tepat setelah spin ini.
     */
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) :: "memory");
    t->state = THREAD_BLOCKED;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    while (t->state == THREAD_BLOCKED) {
        /* IRQ hidup: tick bisa menyela kapan saja. */
    }
}

void sched_wakeup(struct sched_thread *t)
{
    if (t)
        t->state = THREAD_RUNNABLE;
}

void sched_start(void)
{
    uint32_t *f;

    if (nthreads == 0)
        return;
    cur = 0;
    /*
     * Sinkronkan TTBR0 + vm_cur_space ke task thread pertama sebelum
     * rfeia. Mulai titik ini invariant "ruang alamat == vm milik task
     * thread berjalan" dipegang oleh sched_on_tick().
     */
    if (threads[0].task)
        vm_space_switch(&threads[0].task->vm);
    fpu_restore(&threads[0].vfp);   /* start with clean VFP */
    f = threads[0].sp;
    __asm__ volatile(
        "mov sp, %0\n\t"
        "add sp, sp, #4\n\t"       /* skip pad */
        "pop {r0-r12}\n\t"
        "rfeia sp!"                 /* pc=f[14], cpsr=f[15] */
        :: "r"(f) : "memory");
    __builtin_unreachable();
}

uint32_t *sched_on_tick(uint32_t *frame)
{
    /* No VFP use in this function (build flag); the save/restore below
     * captures the outgoing/incoming thread's real VFP registers. */
    struct sched_thread *out = &threads[cur];
    struct sched_thread *in;
    unsigned nxt = cur, i;

    fpu_save(&out->vfp);
    out->sp = frame;
    ticks++;

    /*
     * Round-robin ke thread RUNNABLE berikutnya; yang BLOCKED dilewati.
     * Kalau semua blocked (tidak terjadi di tes ini - selalu ada yang
     * runnable), tetap di thread sekarang agar tidak crash.
     */
    for (i = 0; i < nthreads; i++) {
        nxt++;
        if (nxt >= nthreads)
            nxt = 0;
        if (threads[nxt].state == THREAD_RUNNABLE)
            break;
    }
    if (threads[nxt].state != THREAD_RUNNABLE)
        nxt = cur;
    cur = nxt;
    in = &threads[cur];

    /*
     * Ganti protection domain bila task-nya beda: TTBR0 + TLB flush.
     * Berjalan dalam konteks IRQ (IRQ ter-mask oleh exception itu
     * sendiri), tanpa mask/unmask manual seperti vm_probe - pola yang
     * sama dengan switch eksplisit Fase 5 yang stabil 25/25 run.
     */
    if (in->task && out->task && in->task != out->task)
        vm_space_switch(&in->task->vm);

    fpu_restore(&in->vfp);
    return in->sp;
}

uint32_t *c_irq_handler(uint32_t *frame)
{
    unsigned id = gic_ack();

    if (id == TIMER_PPI_IRQ) {
        /* Reprogram first (clears the level), then EOI, then switch. */
        timer_set_tval(slice_ticks);
        gic_eoi(id);
        return sched_on_tick(frame);
    }
    gic_eoi(id);
    return frame;
}
