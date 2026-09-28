/*
 * pcb.c - PCB initialization (bring-up).
 *
 * A fresh PCB is set up so that ctx_switch's final "bx lr" lands in
 * entry(): lr = entry, sp = thread stack top, r4-r11 = 0.
 */
#include "pcb.h"

void pcb_init(struct pcb *p, void *stack_top, void (*entry)(void))
{
    int i;
    for (i = 0; i < 8; i++)
        p->r4_r11[i] = 0u;
    p->sp = (uint32_t)stack_top;
    p->lr = (uint32_t)entry;
}
