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

/*
 * Real trap stubs for undef / svc / prefetch-abort / data-abort.
 *
 * Each stub:
 *   srsdb sp!, #mode   - push the exception mode's banked LR+SPSR
 *   sub sp, sp, #4     - pad so the frame stays 8-byte aligned (AAPCS)
 *   push {r0-r12}      - save the general registers
 *   arm_trap(exc, regs) with regs -> r0 (matches struct trap_regs)
 *   ... then either RFE back (svc/undef) or park on wfi (aborts).
 *
 * Abort LR adjustments (prefetch: lr-4, data: lr-8) only matter when
 * resuming; the abort stubs record the trap and park, so no fixup yet.
 * IRQ/FIQ paths below are untouched.
 */
    .macro trap_stub name, excnum, mode, ret
    .global \name
    .type \name, %function
\name:
    srsdb sp!, #\mode
    sub sp, sp, #4
    push {r0-r12}
    mov r0, #\excnum
    mov r1, sp
    bl arm_trap
    pop {r0-r12}
    add sp, sp, #4
    .if \ret
    rfefd sp!
    .else
1:  wfi
    b 1b
    .endif
    .endm

    trap_stub undef_handler,    1, 0x1b, 1   /* EXC_UNDEF = 1, UND, return */
    trap_stub svc_handler,      2, 0x13, 1   /* EXC_SVC = 2, SVC, return   */
    trap_stub prefetch_handler, 3, 0x17, 0   /* EXC_PREFETCH_ABORT, hang   */
    trap_stub data_handler,     4, 0x17, 1   /* EXC_DATA_ABORT, pager may return */

    dead_handler reserved_handler
    dead_handler fiq_handler

/*
 * Handler IRQ (preemptif):
 *   - bangun exception frame penuh di stack SVC thread yang terinterupsi:
 *       [pad, r0..r12, pc, cpsr]  (16 word, 8-byte aligned)
 *   - panggil c_irq_handler(frame); ia mengembalikan SP frame thread
 *     berikutnya (bisa sama) — scheduler preemptif terjadi di sini
 *   - rfeia: pc = frame[14], cpsr = frame[15]
 *
 * Catatan: srsdb menyimpan lr_irq-4 (sudah dikurangi) dan spsr_irq ke
 * stack mode SVC, lalu kita pindah ke SVC dan push sisanya.
 */
    .global irq_handler
    .type irq_handler, %function
irq_handler:
    sub lr, lr, #4
    srsdb sp!, #0x13        /* [sp-8]=lr_irq-4, [sp-4]=spsr_irq (stack SVC) */
    cps #0x13              /* SVC: sp sekarang menunjuk frame parsial */
    push {r0-r12}          /* frame: r0..r12 */
    sub sp, sp, #4         /* pad -> total 16 word */
    mov r0, sp
    bl c_irq_handler       /* r0 = frame thread berikutnya */
    mov sp, r0
    add sp, sp, #4         /* lewati pad */
    pop {r0-r12}
    rfeia sp!              /* pc=[sp], cpsr=[sp+4] */
    .size irq_handler, . - irq_handler

    .end