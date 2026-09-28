/*
 * usd.ld - linker script utilitas usd (Fase 15).
 * _start HARUS di awal image: section .text.start di-order pertama.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10050000;   /* USD_PROG_VA, disepakati dengan kernel (user.h) */

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
