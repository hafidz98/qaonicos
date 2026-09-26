/* Minimal entry for the ctx-switch test. */
    .syntax unified
    .arm
    .section .text.boot, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    ldr sp, =_stack_top
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
    .size _start, . - _start
    .end
