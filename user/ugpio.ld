/*
 * ugpio.ld - linker script utilitas ugpio (Fase 14).
 * _start HARUS di awal image: section .text.start di-order pertama.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10048000;   /* UGPIO_PROG_VA, disepakati dengan kernel (user.h) */

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
