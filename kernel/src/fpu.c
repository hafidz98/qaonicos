/*
 * fpu.c - VFPv4 bring-up for Cortex-A7.
 *
 * Compile this file with -mfpu=neon-vfpv4 (softfp): the vmsr/vstmia
 * mnemonics need the assembler to know about VFP, while softfp keeps
 * the integer calling convention used by the rest of the bring-up.
 */
#include "fpu.h"

void fpu_enable(void)
{
    uint32_t v;

    /* CPACR: full access to cp10 and cp11 (bits 23:20 = 0b1111). */
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(v));
    v |= (0xFu << 20);
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 2" :: "r"(v));
    __asm__ volatile("isb");

    /* FPEXC.EN (bit 30): enable the VFP itself. */
    v = (1u << 30);
    __asm__ volatile("vmsr fpexc, %0" :: "r"(v));
}

void fpu_save(struct vfp_state *s)
{
    uint64_t *p = s->d;
    uint32_t fpscr;
    __asm__ volatile(
        "vstmia %0!, {d0-d15}\n\t"
        "vstmia %0!, {d16-d31}\n\t"
        : "+r"(p) :: "memory");
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    s->fpscr = fpscr;
}

void fpu_restore(struct vfp_state *s)
{
    uint64_t *p = s->d;
    __asm__ volatile("vmsr fpscr, %0" :: "r"(s->fpscr));
    __asm__ volatile(
        "vldmia %0!, {d0-d15}\n\t"
        "vldmia %0!, {d16-d31}\n\t"
        : "+r"(p) :: "memory");
}
