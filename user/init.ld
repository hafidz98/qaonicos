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

    /* Orphan .ARM.exidx harus eksplisit: lld kadang menaruhnya
     * SEBELUM .text (entry jadi 0x100010, bukan 0x100000). */
    .ARM.exidx : {
        *(.ARM.exidx*)
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
