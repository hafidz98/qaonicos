/*
 * sched.h - Preemptive round-robin scheduler (bring-up).
 *
 * Timer PPI (virtual timer) fires every slice; the IRQ stub in vectors.S
 * builds a 16-word exception frame on the interrupted thread's SVC stack:
 *   [0]=pad, [1..13]=r0..r12, [14]=pc, [15]=cpsr
 * c_irq_handler() reaps the timer IRQ and calls sched_on_tick(), which
 * saves the outgoing thread's frame + VFP state and returns the incoming
 * thread's frame. Threads never yield manually.
 *
 * IMPORTANT: sched.c is built with -mgeneral-regs-only so the compiler
 * never touches VFP in the IRQ path; the eager fpu_save/fpu_restore in
 * sched_on_tick() then captures the threads' real VFP state.
 */
#ifndef _SCHED_H_
#define _SCHED_H_

#include <stdint.h>
#include "fpu.h"

#define SCHED_MAX_THREADS 4u

/* Exception frame layout (words). */
#define FR_WORDS 16u
#define FR_PC    14u
#define FR_CPSR  15u

struct sched_thread {
    uint32_t *sp;           /* saved exception frame */
    struct vfp_state vfp;   /* saved VFP state */
    int id;
};

void sched_init(void);

/* stack_top must be 8-byte aligned. */
void sched_add(void (*entry)(void), uint8_t *stack_top);

/* Ticks per timer slice, for IRQ reprogramming. */
void sched_set_slice(uint32_t ticks);

/* Jump to thread 0. Never returns. Call with IRQs enabled. */
void sched_start(void);

/* Timer tick: save outgoing, pick next, restore incoming. */
uint32_t *sched_on_tick(uint32_t *frame);

unsigned sched_ticks(void);

/* C IRQ entry called from vectors.S (replaces the old void version). */
uint32_t *c_irq_handler(uint32_t *frame);

#endif /* _SCHED_H_ */
