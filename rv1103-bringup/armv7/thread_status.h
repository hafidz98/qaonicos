/*
 * thread_status.h - ARMv7 thread state for the Mach port.
 *
 * Mirrors the role of i386/thread_status.h: a flavor-tagged CPU
 * register snapshot that the thread/exception code saves and restores.
 */
#ifndef _ARMV7_THREAD_STATUS_H_
#define _ARMV7_THREAD_STATUS_H_

#include <stdint.h>

/*
 * Thread state flavors. Values follow the conventional ARM (Darwin /
 * GNU Mach) numbering so they stay ABI-compatible with user code.
 */
#define ARM_THREAD_STATE            1
#define ARM_VFP_STATE               2
#define ARM_EXCEPTION_STATE         3
#define ARM_DEBUG_STATE             4
#define THREAD_STATE_NONE           5
#define ARM_THREAD_STATE64          6
#define VALID_THREAD_STATE_FLAVOR(x) \
    ((x) == ARM_THREAD_STATE     || (x) == ARM_VFP_STATE || \
     (x) == ARM_EXCEPTION_STATE  || (x) == ARM_DEBUG_STATE)

/*
 * Full general-purpose register snapshot (AArch32).
 *
 * r0..r12 : general purpose registers
 * sp      : stack pointer (r13)
 * lr      : link register  (r14)
 * pc      : program counter (r15)
 * cpsr    : current program status register
 */
struct armv7_thread_state {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t r12;
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
};

/* Number of 32-bit words in struct armv7_thread_state. */
#define ARM_THREAD_STATE_COUNT      17

/* Minimal VFP/floating-point flavor. */
struct armv7_vfp_state {
    uint32_t r[32];     /* d0..d15, 2 words each */
    uint32_t fpscr;
    uint32_t fpsid;
};
#define ARM_VFP_STATE_COUNT         34

/* Exception state recorded by the kernel on a fault. */
struct armv7_exception_state {
    uint32_t far;       /* fault address register  */
    uint32_t fsr;       /* fault status register   */
    uint32_t exception; /* ARM exception vector    */
};
#define ARM_EXCEPTION_STATE_COUNT   3

#endif /* _ARMV7_THREAD_STATUS_H_ */