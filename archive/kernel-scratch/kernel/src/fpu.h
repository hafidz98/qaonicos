/* fpu.h - VFPv4 enable + context save/restore (bring-up). */
#ifndef _FPU_H_
#define _FPU_H_

#include <stdint.h>

/* Full VFPv4 context: 32 double regs + FPSCR. 8-byte aligned: vstmia
 * faults on unaligned addresses when alignment checking is on. */
struct vfp_state {
    uint64_t d[32];
    uint32_t fpscr;
} __attribute__((aligned(8)));

/* Enable VFP/NEON: CPACR full access for cp10/cp11 + FPEXC.EN. */
void fpu_enable(void);

/* Eager save/restore of the whole VFP context. */
void fpu_save(struct vfp_state *s);
void fpu_restore(struct vfp_state *s);

#endif /* _FPU_H_ */
