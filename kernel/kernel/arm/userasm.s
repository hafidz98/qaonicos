/*
 * mach3/kernel/arm/userasm.s -- user mode entry/exit (M4 item 4).
 *
 * unsigned int user_enter_test(unsigned int user_sp, unsigned int user_pc):
 *   Set the USR-mode SP, then "return from exception" into user mode.
 *   Does NOT return normally; control comes back via user_exit_trampoline
 *   with r0 = exit/fault code (set by the trap handler redirecting the
 *   trap frame's lr/spsr).
 *
 * void user_exit_trampoline(void):
 *   Entered from arm_trap_handler via a redirected trap frame
 *   (SVC mode, r0 = code).  Restores the test's kernel SP and returns
 *   to user_enter_test's caller.
 */
	.text
	.align 2

	.globl user_enter_test
	.type user_enter_test, %function
user_enter_test:
	/* r0 = user_sp, r1 = user_pc.
	 * Fase C: save/restore SEMUA register callee-saved (r4-r11),
	 * bukan hanya r4.  Trampoline bukan return normal — ia harus
	 * me-restore semuanya agar compiler bebas menaruh variabel
	 * live di r5-r11 melewati panggilan ini.
	 * Catatan: 10 register (40 byte) agar stack tetap 8-byte
	 * aligned sesuai ARM EABI (9 register = 36 byte = misaligned!). */
	push	{r4-r12, lr}
	ldr	r4, =_user_test_ksp
	str	sp, [r4]		/* save kernel sp (after push) */

	/*
	 * Set the USR-mode stack pointer.  We can't use USR mode itself:
	 * CPS cannot change mode from USR (the cps #0x13 would be ignored,
	 * leaving the RFE below to execute undefined in USR).  SYS mode
	 * (0x1f) is privileged yet shares r13/r14 with USR.
	 */
	cpsid	i, #0x1f		/* SYS: privileged, USR-banked sp/lr */
	mov	sp, r0			/* user stack */
	cpsid	i, #0x13		/* back to SVC */

	/*
	 * Fake an exception return frame on the SVC stack:
	 * [sp] = user_pc, [sp+4] = USR spsr.  rfefd loads PC then CPSR.
	 * CPSR=0x10 re-enables IRQs on entry to user mode.
	 */
	mov	r2, #0x10
	push	{r2}
	push	{r1}
	rfefd	sp!			/* -> user mode */
	/* NOTREACHED */
1:	b	1b
	.size user_enter_test, .-user_enter_test

	.globl user_exit_trampoline
	.type user_exit_trampoline, %function
user_exit_trampoline:
	/* r0 = exit/fault code.  SVC mode, IRQs on (spsr was 0x13).
	 * Restore semua callee-saved (simetris dengan push di atas:
	 * 10 register agar 8-byte aligned). */
	ldr	r1, =_user_test_ksp
	ldr	sp, [r1]		/* restore test's kernel sp */
	mov	r2, r0			/* save code across pop */
	pop	{r4-r12, lr}
	mov	r0, r2
	bx	lr			/* return to test, r0 = code */
	.size user_exit_trampoline, .-user_exit_trampoline

/*
 * ctx_save_usr / ctx_restore_usr -- App A2 (cooperative user threads).
 *
 * void ctx_save_usr(unsigned int *sp_out, unsigned int *lr_out):
 *   read the USR-mode banked sp/lr (via SYS mode, which shares them).
 * void ctx_restore_usr(unsigned int sp, unsigned int lr):
 *   write the USR-mode banked sp/lr.
 * Called from SVC mode with IRQs already disabled (trap entry).
 */
	.globl ctx_save_usr
	.type ctx_save_usr, %function
ctx_save_usr:
	cpsid	i, #0x1f		/* SYS: privileged, USR-banked sp/lr */
	str	sp, [r0]
	str	lr, [r1]
	cpsid	i, #0x13		/* back to SVC */
	bx	lr
	.size ctx_save_usr, .-ctx_save_usr

	.globl ctx_restore_usr
	.type ctx_restore_usr, %function
ctx_restore_usr:
	cpsid	i, #0x1f
	mov	sp, r0
	mov	lr, r1
	cpsid	i, #0x13
	bx	lr
	.size ctx_restore_usr, .-ctx_restore_usr

	.bss
	.align 2
_user_test_ksp:
	.space	4
