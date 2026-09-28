/*
 * init.ld - linker script program init userspace (Fase B).
 *
 * Di-link di INIT_CODE_VA (0x100000), disepakati dengan kernel/user.c.
 * _start HARUS di awal image: section .text.start di-order pertama.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x100000;   /* INIT_CODE_VA */

    .text : {
        *(.text.start)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }
}
