/* Minimal entry for the FPU test. */
    .syntax unified
    .arm
    .section .text.boot, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    ldr sp, =_stack_top

    /* Harness prerequisite: enable VFP before ANY VFP instruction can
     * execute. main() is compiled with -mfpu, so even its prologue may
     * contain VFP ops (vpush). The real fpu_enable() API is still
     * exercised and verified from C below. */
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #0xF00000
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #0x40000000
    vmsr fpexc, r0

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
