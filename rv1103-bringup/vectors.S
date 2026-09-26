/*
 * vectors.S - Tabel vektor + IRQ dispatch skeleton ARMv7-A untuk RV1103
 *             (Cortex-A7, ARMv7-A).
 *
 * Tabel 8 entry, masing-masing `ldr pc, [pc, #24]` (literal pool tepat
 * setelah tabel). Dengan layout ini offset-nya konstan 0x18:
 *   entry ke-k di 4*k, effective PC = 4*k + 8, literal ke-k di 0x20 + 4*k,
 *   sehingga offset = (0x20 + 4*k) - (4*k + 8) = 0x18 = 24.
 *
 * Section .vectors harus 32-byte aligned (syarat VBAR). Alamatnya ditulis
 * ke VBAR oleh irq_init() (lihat irq.c). SP mode IRQ memakai simbol
 * irq_stack_top yang disediakan linker script.
 */

    .section .vectors, "ax"
    .arm
    .align 5
    .global _vectors
_vectors:
    ldr pc, [pc, #24]       /* 0x00 reset          -> _start            */
    ldr pc, [pc, #24]       /* 0x04 undefined      -> undef_handler     */
    ldr pc, [pc, #24]       /* 0x08 supervisor     -> svc_handler       */
    ldr pc, [pc, #24]       /* 0x0c prefetch abort -> prefetch_handler  */
    ldr pc, [pc, #24]       /* 0x10 data abort     -> data_handler      */
    ldr pc, [pc, #24]       /* 0x14 reserved       -> reserved_handler  */
    ldr pc, [pc, #24]       /* 0x18 IRQ            -> irq_handler       */
    ldr pc, [pc, #24]       /* 0x1c FIQ            -> fiq_handler       */

    /* Literal pool: satu word per entry, urut sesuai tabel di atas. */
    .word reset_handler
    .word undef_handler
    .word svc_handler
    .word prefetch_handler
    .word data_handler
    .word reserved_handler
    .word irq_handler
    .word fiq_handler

/*
 * Handler reset: lompat ke simbol eksternal _start (startup C runtime).
 * _start diasumsikan sudah menyiapkan stack tiap mode, termasuk mode IRQ.
 */
    .global reset_handler
reset_handler:
    b _start

/*
 * Makro handler "mati": hentikan CPU sampai interrupt berikutnya.
 */
    .macro dead_handler name
    .global \name
\name:
1:  wfi
    b 1b
    .endm

    dead_handler undef_handler
    dead_handler svc_handler
    dead_handler prefetch_handler
    dead_handler data_handler
    dead_handler reserved_handler
    dead_handler fiq_handler

/*
 * Handler IRQ:
 *   - pasang SP mode IRQ dari irq_stack_top (disediakan linker script)
 *   - simpan r0-r3, r12, lr (volatile + return address)
 *   - panggil c_irq_handler() (C, AAPCS: r4-r11 dijaga oleh callee)
 *   - restore
 *   - subs pc, lr, #4  -> return ke instruksi yang terinterupsi,
 *     sekaligus memulihkan CPSR (mode + I-bit) dari SPSR.
 *
 * Catatan: lr pada entry IRQ bernilai return address + 4, sehingga
 * lr - 4 adalah alamat instruksi yang harus dieksekusi ulang.
 */
    .global irq_handler
    .type irq_handler, %function
irq_handler:
    ldr sp, =irq_stack_top
    push {r0-r3, r12, lr}
    bl c_irq_handler
    pop {r0-r3, r12, lr}
    subs pc, lr, #4
    .size irq_handler, . - irq_handler

    .end