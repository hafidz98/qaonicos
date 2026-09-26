/*
 * sched.c - Preemptive round-robin scheduler.
 *
 * BUILD WITH -mgeneral-regs-only (see sched.h for why).
 */
#include "sched.h"
#include "gic.h"
#include "timer.h"

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

void sched_add(void (*entry)(void), uint8_t *stack_top)
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

void sched_start(void)
{
    uint32_t *f;

    if (nthreads == 0)
        return;
    cur = 0;
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
    fpu_save(&threads[cur].vfp);
    threads[cur].sp = frame;
    ticks++;
    cur++;
    if (cur >= nthreads)
        cur = 0;
    fpu_restore(&threads[cur].vfp);
    return threads[cur].sp;
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
