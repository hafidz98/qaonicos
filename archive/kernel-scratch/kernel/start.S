/* Entry for the integrated kernel demo:
 * VBAR, VFP enable (kernel_main uses doubles), per-mode stacks,
 * bss, then kernel_main(). */
    .syntax unified
    .arm
    .section .text.boot, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    /* Vector table. */
    ldr r0, =_vectors
    mcr p15, 0, r0, c12, c0, 0
    isb

    /* VFP on early: kernel_main() and threads execute VFP instructions,
     * and even a function prologue may contain them when compiled
     * with -mfpu. (The fpu_enable() API is still exercised from C.) */
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #0xF00000
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #0x40000000
    vmsr fpexc, r0

    /* Per-mode stacks. */
    ldr sp, =_stack_top            /* SVC */
    cps #0x1b
    ldr sp, =_und_stack_top        /* UND */
    cps #0x17
    ldr sp, =_abt_stack_top        /* ABT */
    cps #0x13                     /* back to SVC */

    /* Zero .bss. */
    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
    b 2f
1:  str r2, [r0], #4
2:  cmp r0, r1
    blo 1b

    bl kernel_main
_hang:
    wfi
    b _hang

    .size _start, . - _start
    .end
