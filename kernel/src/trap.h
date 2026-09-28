/* trap.h - ARMv7 exception dispatch to C (bring-up). */
#ifndef _TRAP_H_
#define _TRAP_H_

#include <stdint.h>

/*
 * Register frame built by the asm vector stubs. Layout (r1 on entry):
 *   r[0..12]  : r0-r12 at trap time
 *   _pad      : 4-byte gap from the stub's "sub sp, sp, #4"
 *   lr        : banked LR of the exception mode (srsdb stores LR at
 *               [sp] and SPSR at [sp+4]; rfe is its exact inverse)
 *   spsr      : banked SPSR of the exception mode
 */
struct trap_regs {
    uint32_t r[13];
    uint32_t _pad;
    uint32_t lr;
    uint32_t spsr;
};

/* C entry point called from vectors.S: exc = EXC_* slot. */
void arm_trap(unsigned exc, struct trap_regs *regs);

/* Bring-up observability. */
extern volatile unsigned trap_count[8];
extern volatile unsigned last_trap_exc;
extern volatile unsigned svc_last_num;   /* r7 at last SVC */

#endif /* _TRAP_H_ */
