/*
 * start.S - RV1103 bare-metal reset / entry stub.
 *
 * Entered by U-Boot via 'go' at the DRAM base (0x00000000). The MMU
 * and caches are left as the bootloader configured them; this stub
 * only sets up the stacks, zeroes .bss and calls main().
 *
 * Cortex-A7, ARM (A32) mode, no libc.
 */

    .syntax unified
    .arm

    .section .text.boot, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    /* Initial supervisor (SVC) stack, at the top of RAM. */
    ldr sp, =_stack_top

    /* Switch to IRQ mode and install the dedicated 8 KB IRQ stack
     * (reserved by the linker script for the T4 IRQ handler). */
    cps #0x12
    ldr sp, =irq_stack_top

    /* Fase 9: install ABT + UND stacks. WAJIB: stub data/prefetch
     * abort (vectors.S: srsdb sp!, #0x17) dan undef (srsdb #0x1b)
     * memakai sp_abt/sp_und. Tanpa ini sp_abt/sp_und berisi sampah
     * reset -> push 64 byte + call chain C arm_trap/user_kill menimpa
     * BSS (nthreads/cur/threads[]) secara non-deterministik. Gejala:
     * cur menjadi 10 (>= nthreads) lalu abort di sched_on_tick.
     * Bug laten sejak Fase 7 (pager pakai data abort); Fase 9
     * menggeser layout BSS sehingga korupsi mengenai cur. */
    cps #0x17
    ldr sp, =_abt_stack_top
    cps #0x1b
    ldr sp, =_und_stack_top

    /* Back to supervisor mode. */
    cps #0x13

    /* Zero the .bss region (_bss_start .. _bss_end). */
    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
    b   2f
1:
    str r2, [r0], #4
2:
    cmp r0, r1
    blo 1b

    /* C entry point. */
    bl main

    /* main() is not expected to return; park the core. */
3:
    wfi
    b 3b

    .size _start, . - _start
    .end