/*
 * rv1103.ld - Linker script for the RV1103 bare-metal bring-up image.
 *
 * DRAM base 0x00000000, 64 MB (per hw-addrs.md). The image is loaded
 * by U-Boot via 'go' straight to the entry point (_start), so the
 * link address equals the load address (0x0).
 *
 * Stacks live at the very top of RAM:
 *   _stack_top     = 0x04000000  (SVC / supervisor stack)
 *   irq_stack_top  = 0x03ffe000  (8 KB reserved for the IRQ stack, T4)
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64M
}

/* Top of RAM, used as the initial (SVC) stack pointer. */
_stack_top = ORIGIN(RAM) + LENGTH(RAM);

/* IRQ-mode stack: 8 KB carved out just below the SVC stack. */
irq_stack_top = _stack_top - 0x2000;

SECTIONS
{
    . = ORIGIN(RAM);

    /* Boot entry must be first so _start lands exactly at 0x0. */
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        *(.rodata1)
    } > RAM

    .data : ALIGN(4) {
        *(.data .data.*)
        *(.data1)
    } > RAM

    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } > RAM
}