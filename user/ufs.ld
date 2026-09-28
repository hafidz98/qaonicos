/*
 * ufs.ld - linker script utilitas ufs (Fase 16).
 * _start HARUS di awal image: section .text.start di-order pertama.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10058000;   /* UFS_PROG_VA, disepakati dengan kernel (user.h) */

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
