/*
 * exception.h - ARMv7 exception vectors and saved-state description.
 *
 * The values below are the ARMv7-A exception vector offsets divided by
 * four (i.e. the vector slot index 0..7), which is how the trap code
 * indexes the vector table.
 */
#ifndef _ARMV7_EXCEPTION_H_
#define _ARMV7_EXCEPTION_H_

#include <stdint.h>

/* ARMv7-A exception vector slots. */
#define EXC_RESET           0   /* 0x00 : reset                    */
#define EXC_UNDEF           1   /* 0x04 : undefined instruction    */
#define EXC_SVC             2   /* 0x08 : supervisor call          */
#define EXC_PREFETCH_ABORT  3   /* 0x0c : prefetch abort           */
#define EXC_DATA_ABORT      4   /* 0x10 : data abort               */
#define EXC_RESERVED        5   /* 0x14 : reserved / hyp trap      */
#define EXC_IRQ             6   /* 0x18 : IRQ                      */
#define EXC_FIQ             7   /* 0x1c : FIQ                      */

#define EXC_VECTOR_COUNT    8

/* Byte offset of a vector slot inside the vector table. */
#define EXC_VECTOR_OFFSET(n)    ((n) * 4u)

/* CPSR mode bits (M[4:0]) relevant to exception entry. */
#define ARM_CPSR_MODE_MASK  0x1fu
#define ARM_MODE_USR        0x10u
#define ARM_MODE_FIQ        0x11u
#define ARM_MODE_IRQ        0x12u
#define ARM_MODE_SVC        0x13u
#define ARM_MODE_ABT        0x17u
#define ARM_MODE_UND        0x1bu
#define ARM_MODE_SYS        0x1fu

/*
 * Minimal saved state captured on exception entry.
 *
 * r[]   : r0..r12, exactly as banked on entry
 * sp    : banked stack pointer of the trapping mode
 * lr    : banked link register (return address / SVC argument)
 * pc    : resume address
 * cpsr  : status at the time of the exception
 * exc   : EXC_* slot that produced this frame
 */
struct armv7_saved_state {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
    uint32_t exc;
};

#endif /* _ARMV7_EXCEPTION_H_ */