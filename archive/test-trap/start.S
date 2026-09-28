/* Entry for the trap test: VBAR, per-mode stacks, bss, then main(). */
    .syntax unified
    .arm
    .section .text.boot, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    /* Point VBAR at our vector table (in SVC/PL1, allowed). */
    ldr r0, =_vectors
    mcr p15, 0, r0, c12, c0, 0
    isb

    /* Stacks for the modes this test exercises. */
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

    bl main
_hang:
    wfi
    b _hang

/* Dummy: IRQs are never enabled in this test, but vectors.S references it. */
    .global c_irq_handler
    .type c_irq_handler, %function
c_irq_handler:
    b c_irq_handler
    .size _start, . - _start
    .end
