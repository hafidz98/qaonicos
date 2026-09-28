/*
 * mach3/kernel/arm/locore.s -- ARMv7 boot and exception vectors.
 *
 * QEMU -M virt: loaded at 0x40000000 (ELF e_entry = _start).
 * Sets up an identity-mapped L1 (RAM 0x40000000-0x44000000 as normal WB,
 * devices 0x08000000-0x0B000000 as device memory), enables the MMU,
 * then calls arm_init().
 *
 * Exception model: all handlers use SRS to build a trap frame on the
 * SVC stack and call arm_trap_handler(frame, trapno).
 */
	.section .text.boot, "ax"
	.align 2

	.globl _start
	.globl _vectors

/* ------------------------------------------------------------------ */
/* Vector table.  Must be at the ELF entry point.                      */
/* ------------------------------------------------------------------ */
_vectors:
_start:
	b	_reset			/* 0x00 reset */
	b	_undef_handler		/* 0x04 undefined instruction */
	b	_svc_handler		/* 0x08 supervisor call */
	b	_pabt_handler		/* 0x0c prefetch abort */
	b	_dabt_handler		/* 0x10 data abort */
	b	.			/* 0x14 reserved */
	b	_irq_handler		/* 0x18 irq */
	b	_fiq_handler		/* 0x1c fiq */

	.text
/* Trap numbers for arm_trap_handler */
	.set TRAP_UNDEF, 0
	.set TRAP_SVC,   1
	.set TRAP_PABT,  2
	.set TRAP_DABT,  3
	.set TRAP_IRQ,   4
	.set TRAP_FIQ,   5

/* ------------------------------------------------------------------ */
/* Reset / MMU bootstrap                                               */
/* ------------------------------------------------------------------ */
_reset:
	/* Set VBAR to our vector table */
	ldr	r0, =_vectors
	mcr	p15, 0, r0, c12, c0, 0	/* VBAR */

	/* Start in SVC mode, IRQ+FIQ disabled */
	cpsid	if, #0x13

	/* Invalidate I/D caches, branch predictor, TLB */
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0	/* ICIALLU */
	mcr	p15, 0, r0, c7, c6, 0	/* BPIALL */
	mcr	p15, 0, r0, c8, c7, 0	/* TLBIALL */
	dsb
	isb

	/* Set up mode stacks (all in BSS, identity-mapped) */
	ldr	sp, =_svc_stack_top
	cps	#0x12			/* IRQ mode */
	ldr	sp, =_irq_stack_top
	cps	#0x11			/* FIQ mode */
	ldr	sp, =_fiq_stack_top
	cpsid	if, #0x13		/* back to SVC */

	/* Clear BSS */
	ldr	r0, =_bss_start
	ldr	r1, =_bss_end
	mov	r2, #0
1:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	1b

	/* Build the L1 page table (in BSS, 16KB aligned) */
	bl	_build_l1

	/* TTBR0 = L1 base, outer write-back */
	ldr	r0, =_l1_table
	orr	r0, r0, #0x08		/* TTBR0[5:3] = 0b001 (outer WB) */
	mcr	p15, 0, r0, c2, c0, 0	/* TTBR0 */

	/* DACR: all 16 domains = client (0b01) */
	ldr	r0, =0x55555555
	mcr	p15, 0, r0, c3, c0, 0	/* DACR */

	/* Enable MMU, I/D cache, branch prediction, alignment check */
	mrc	p15, 0, r0, c1, c0, 0	/* SCTLR */
	orr	r0, r0, #(1<<0)		/* M:  MMU enable */
	orr	r0, r0, #(1<<1)		/* A:  alignment check */
	orr	r0, r0, #(1<<2)		/* C:  data cache */
	orr	r0, r0, #(1<<11)	/* Z:  branch prediction */
	orr	r0, r0, #(1<<12)	/* I:  instruction cache */
	mcr	p15, 0, r0, c1, c0, 0	/* SCTLR */
	isb

	/* Hand off to C.  Identity-mapped, so no far jump needed. */
	bl	arm_init

	/* arm_init should not return; halt if it does. */
1:	wfi
	b	1b

/* ------------------------------------------------------------------ */
/* _build_l1: fill the 16KB L1 table.                                  */
/*   RAM  0x40000000-0x44000000 : 64 x 1MB sections, normal WB, AP=rw   */
/*   DEV  0x08000000-0x0B000000 : 33 x 1MB sections, device, XN, AP=rw  */
/* Clobbers r0-r4.                                                     */
/* ------------------------------------------------------------------ */
_build_l1:
	ldr	r0, =_l1_table
	mov	r1, #0
	mov	r2, #4096
1:	str	r1, [r0], #4		/* zero 4096 entries */
	subs	r2, r2, #1
	bne	1b

	ldr	r0, =_l1_table

	/* RAM sections: descriptor = base | 0x14C0E
	 * (S=1, TEX=001, AP=0b11, C=1, B=1, domain 0, section) */
	ldr	r1, =0x40000000		/* section base */
	ldr	r2, =0x14C0E		/* attributes */
	mov	r3, #64			/* 64 sections = 64 MB */
	add	r4, r0, r1, lsr #18	/* L1 index = va >> 20; entry addr */
1:	orr	r5, r1, r2
	str	r5, [r4], #4
	add	r1, r1, #0x100000
	subs	r3, r3, #1
	bne	1b

	/* Device sections: descriptor = base | 0x10C12
	 * (S=1, TEX=000, AP=0b11, XN=1, C=0, B=0, domain 0, section) */
	ldr	r1, =0x08000000
	ldr	r2, =0x10C12
	mov	r3, #33			/* 33 sections = 33 MB (covers virtio-mmio @0x0a000000) */
	add	r4, r0, r1, lsr #18
1:	orr	r5, r1, r2
	str	r5, [r4], #4
	add	r1, r1, #0x100000
	subs	r3, r3, #1
	bne	1b

	mov	pc, lr

/* ------------------------------------------------------------------ */
/* Exception handlers.  Build trap frame on SVC stack via SRS,         */
/* then call arm_trap_handler(frame, trapno).                          */
/* Frame layout (struct arm_trap_frame): r0-r12, lr (return addr), spsr */
/* ------------------------------------------------------------------ */
_undef_handler:
	sub	lr, lr, #4
	srsdb	sp!, #0x13
	cps	#0x13
	push	{r0-r12}
	mov	r0, sp
	mov	r1, #TRAP_UNDEF
	bl	arm_trap_handler
	pop	{r0-r12}
	rfefd	sp!

_svc_handler:
	sub	lr, lr, #4
	srsdb	sp!, #0x13
	cps	#0x13
	push	{r0-r12}
	mov	r0, sp
	mov	r1, #TRAP_SVC
	bl	arm_trap_handler
	pop	{r0-r12}
	rfefd	sp!

_pabt_handler:
	sub	lr, lr, #4
	srsdb	sp!, #0x13
	cps	#0x13
	push	{r0-r12}
	mov	r0, sp
	mov	r1, #TRAP_PABT
	bl	arm_trap_handler
	pop	{r0-r12}
	rfefd	sp!

_dabt_handler:
	sub	lr, lr, #8
	srsdb	sp!, #0x13
	cps	#0x13
	push	{r0-r12}
	mov	r0, sp
	mov	r1, #TRAP_DABT
	bl	arm_trap_handler
	pop	{r0-r12}
	rfefd	sp!

_irq_handler:
	sub	lr, lr, #4
	srsdb	sp!, #0x13
	cps	#0x13
	push	{r0-r12}
	mov	r0, sp
	mov	r1, #TRAP_IRQ
	bl	arm_trap_handler
	pop	{r0-r12}
	rfefd	sp!

_fiq_handler:
	sub	lr, lr, #4
	srsdb	sp!, #0x13
	cps	#0x13
	push	{r0-r12}
	mov	r0, sp
	mov	r1, #TRAP_FIQ
	bl	arm_trap_handler
	pop	{r0-r12}
	rfefd	sp!

/* ------------------------------------------------------------------ */
/* BSS: L1 table (16KB aligned), mode stacks.                          */
/* ------------------------------------------------------------------ */
	.bss
	.align 14
	.globl _l1_table
_l1_table:
	.space 16384

	.align 12
_svc_stack:
	.space 4096
_svc_stack_top:
_irq_stack:
	.space 4096
_irq_stack_top:
_fiq_stack:
	.space 4096
_fiq_stack_top:
