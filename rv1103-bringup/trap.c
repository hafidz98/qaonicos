/*
 * trap.c - ARMv7 exception dispatch (bring-up).
 *
 * Records every trap; SVC additionally latches the r7 "syscall number".
 * No libc, -ffreestanding.
 */
#include "trap.h"
#include "armv7/exception.h"

volatile unsigned trap_count[8];
volatile unsigned last_trap_exc;
volatile unsigned svc_last_num;

void arm_trap(unsigned exc, struct trap_regs *regs)
{
    if (exc < EXC_VECTOR_COUNT) {
        trap_count[exc]++;
        last_trap_exc = exc;
    }
    if (exc == EXC_SVC && regs) {
        svc_last_num = regs->r[7];
    }
}
