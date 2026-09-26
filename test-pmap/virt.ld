/* Linker script for the pmap test: QEMU virt, DRAM at 0x40000000. */
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
    /* 64 KiB stack, well inside the first mapped section. */
    . = ALIGN(16);
    _stack_top = _BASE + 0x10000;
}
