/*
 * fstest.ld - linker script program uji ramfs.
 * _start HARUS di awal image: section .text.start di-order pertama.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10028000;   /* FSTEST_PROG_VA, disepakati dengan kernel (user.h) */

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
