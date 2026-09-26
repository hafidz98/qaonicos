/* Linker script: qemu-system-arm -M virt, RAM di 0x40000000. */
ENTRY(_start)

MEMORY
{
    RAM : ORIGIN = 0x40000000, LENGTH = 64M
}

SECTIONS
{
    . = 0x40000000;

    .text.boot : {
        *(.text.boot)
    }

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }

    /* Stack tumbuh ke bawah dari ujung atas RAM */
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
