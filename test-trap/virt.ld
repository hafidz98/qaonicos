/* Linker script for the trap test: QEMU virt, DRAM at 0x40000000. */
ENTRY(_start)

_BASE = 0x40000000;

SECTIONS
{
    . = _BASE;
    .vectors : { KEEP(*(.vectors)) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }
    . = ALIGN(16);
    _stack_top     = _BASE + 0x10000;   /* SVC */
    _und_stack_top = _BASE + 0x12000;   /* UND */
    _abt_stack_top = _BASE + 0x14000;   /* ABT */
    irq_stack_top  = _BASE + 0x20000;   /* IRQ (unused here) */
}
