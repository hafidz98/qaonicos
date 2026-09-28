/*
 * mach3/kernel/arm/context.s -- ARM context switch primitives.
 *
 * struct pcb layout (machine/thread.h):
 *   0x00: iss (struct arm_saved_state = 23 words)
 *   0x5C: kss (struct arm_kernel_state: r4-r11, sp, lr)
 *
 * pcb.c's switch_context() extracts pcbs and calls:
 *   thread_t __Switch_context(pcb_t old_pcb, continuation_t cont,
 *                             pcb_t new_pcb, thread_t new_th,
 *                             thread_t old_th)
 */
	.text
	.align 2

	.set PCB_KSS, 92

/*
 * __Switch_context: switch kernel context.
 *   r0 = old_pcb (NULL: skip save)
 *   r1 = continuation (non-NULL: discard old state)
 *   r2 = new_pcb
 *   r3 = new_thread
 *   [sp] = old_thread (5th arg)
 * Returns old_thread in r0 to the resumed thread's resumption point.
 * Never "returns" in the normal sense: always jumps to the new
 * thread's saved resumption PC.
 */
	.globl __Switch_context
	.type __Switch_context, %function
__Switch_context:
	/* Preserve what we need across the switch on the current stack */
	push	{r2, r3, lr}		/* new_pcb, new_thread, retaddr */
					/* [sp,#12] = old_thread */

	cmp	r1, #0
	bne	.Lsw_nosave
	cmp	r0, #0
	beq	.Lsw_nosave

	/* Save old thread's callee-saved state into old_pcb->kss */
	add	r12, r0, #PCB_KSS
	stmia	r12!, {r4-r11}		/* kss.r4-r11; r12 = &kss.sp */
	add	r0, sp, #12		/* caller's sp (before our push) */
	str	r0, [r12], #4		/* kss.sp */
	ldr	r0, [sp, #8]		/* our return address */
	str	r0, [r12]		/* kss.lr */

.Lsw_nosave:
	/* Load everything needed into regs BEFORE switching sp */
	ldr	r0, [sp, #0]		/* new_pcb */
	ldr	r1, [sp, #4]		/* new_thread */
	ldr	r2, [sp, #12]		/* old_thread (return value) */
	add	r0, r0, #PCB_KSS
	ldmia	r0, {r4-r11}		/* new thread's r4-r11 */
	ldr	r12, [r0, #32]		/* kss.sp */
	ldr	lr, [r0, #36]		/* kss.lr = resumption pc */

	/* active_threads[0] = new_thread */
	ldr	r0, =active_threads
	str	r1, [r0]

	/* Switch stack, set return value, jump to resumption point */
	mov	sp, r12
	mov	r0, r2
	bx	lr
	.size __Switch_context, .-__Switch_context

/*
 * void _load_context(pcb_t pcb, thread_t thread)
 * Enter a thread for the first time.  Jumps to the thread's saved
 * resumption PC (thread_continue) with r0 = THREAD_NULL.
 * Enables IRQs (MI cpu_launch_first_thread runs with splhigh).
 */
	.globl _load_context
	.type _load_context, %function
_load_context:
	/* r0 = pcb, r1 = thread */
	add	r2, r0, #PCB_KSS
	ldmia	r2, {r4-r11}
	ldr	r12, [r2, #32]		/* kss.sp */
	ldr	lr, [r2, #36]		/* kss.lr */

	/* active_threads[0] = thread */
	ldr	r2, =active_threads
	str	r1, [r2]

	/* Enable IRQ+FIQ for the new thread (cpsie if) */
	cpsie	if

	/*
	 * M4: Wait for the first timer tick.  The ARM generic timer is
	 * one-shot; MI thread setup (start_kernel_threads) may take
	 * longer than the 10ms initial period, losing the first IRQ
	 * forever.  Spinning here (with IRQs enabled and active_threads
	 * valid) guarantees the timer mechanism is live before proceeding.
	 * ~1e9 loops ≈ 100ms on QEMU TCG.
	 */
	ldr	r2, =0x3B9ACA00
1:	subs	r2, r2, #1
	bne	1b

	mov	sp, r12
	mov	r0, #0			/* THREAD_NULL for thread_continue */
	bx	lr
	.size _load_context, .-_load_context

/*
 * void call_continuation(void (*cont)(void))
 * Reset sp to the top of the current kernel stack and jump to cont.
 */
	.globl call_continuation
	.type call_continuation, %function
call_continuation:
	/* r0 = continuation */
	mov	r1, sp
	lsr	r1, r1, #13
	lsl	r1, r1, #13		/* stack base (8KB aligned) */
	add	r1, r1, #0x1F00
	add	r1, r1, #0xF0
	mov	sp, r1			/* top - 16 */
	bx	r0
	.size call_continuation, .-call_continuation
