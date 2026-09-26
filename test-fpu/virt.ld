/* Linker script for the FPU test: QEMU virt, DRAM at 0x40000000. */
ENTRY(_start)

_BASE = 0x40000000;

SECTIONS
{
    . = _BASE;
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
    _stack_top = _BASE + 0x10000;
}
