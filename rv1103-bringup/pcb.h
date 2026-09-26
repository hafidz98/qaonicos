/* pcb.h - Process Control Block + context switch (bring-up). */
#ifndef _PCB_H_
#define _PCB_H_

#include <stdint.h>

/*
 * Minimal PCB: only what ctx_switch saves/restores.
 *   r4_r11 : callee-saved registers (offsets 0..28, must match switch.S)
 *   sp     : offset 32
 *   lr     : offset 36
 */
struct pcb {
    uint32_t r4_r11[8];
    uint32_t sp;
    uint32_t lr;
};

/* Prepare pcb so the first ctx_switch to it starts executing entry(). */
void pcb_init(struct pcb *p, void *stack_top, void (*entry)(void));

/*
 * Save current context into *old, restore *new_pcb, resume it.
 * Implemented in switch.S. Never returns to the caller directly;
 * it resumes wherever *new_pcb was last suspended (or entry() the
 * first time).
 */
void ctx_switch(struct pcb *old, struct pcb *new_pcb);

#endif /* _PCB_H_ */
