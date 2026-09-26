    .section .text.boot
    .global _start
    .align 2
    .arm
_start:
    /* Set up stack (top of RAM, defined in linker script) */
    ldr sp, =_stack_top

    /* Clear .bss */
    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
bss_loop:
    cmp r0, r1
    strlt r2, [r0], #4
    blt bss_loop

    /* Jump to C */
    bl main

hang:
    wfi
    b hang
