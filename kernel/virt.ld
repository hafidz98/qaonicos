/* Linker script for the integrated kernel: QEMU virt, DRAM at 0x40000000. */
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
    . = ALIGN(4096);
    /* Stack di atas BSS (bukan alamat fix): BSS bisa membesar
     * (mis. pool 1MB ramfs Fase 9) dan menimpa stack bila fix.
     * Urutan: SVC 64KB, UND 8KB, ABT 8KB, IRQ 64KB. */
    _stack_top     = . + 0x10000;          /* SVC */
    _und_stack_top = _stack_top + 0x2000;  /* UND */
    _abt_stack_top = _und_stack_top + 0x2000; /* ABT */
    irq_stack_top  = _abt_stack_top + 0x10000; /* IRQ */
}
