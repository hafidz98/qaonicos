/*
 * mach3/kernel/arm/uprog.s -- embedded first user programs (M4 item 4).
 *
 * Position-independent ARMv7 code (adr for data), copied to a user page
 * at boot by user_selftest().  Syscall ABI: number in r7, args in
 * r0-r2, return in r0; svc #0.
 *
 *   SYS_WRITE = 20 : write(fd, buf, len) -> bytes written
 *   SYS_EXIT  = 22 : exit(code)           -> never returns
 */
	.section .rodata
	.align 2

	.globl _uprog_start
	.globl _uprog_end
	.globl _uprog_fault

/* Clean test: WRITE a message, then EXIT(0). */
_uprog_start:
	mov	r7, #20			/* SYS_WRITE */
	mov	r0, #1			/* fd (console) */
	adr	r1, _umsg
	ldr	r2, =_umsg_len
	svc	#0
	mov	r7, #22			/* SYS_EXIT */
	mov	r0, #0			/* exit code */
	svc	#0
1:	b	1b			/* should not reach */

/* Fault test: WRITE a message, then touch unmapped 0x0 (data abort).
 * The kernel must kill just this user context, not panic. */
_uprog_fault:
	mov	r7, #20			/* SYS_WRITE */
	mov	r0, #1
	adr	r1, _umsg2
	ldr	r2, =_umsg2_len
	svc	#0
	mov	r3, #0
	ldr	r3, [r3]		/* data abort here */
	/* NOTREACHED */
	mov	r7, #22
	mov	r0, #99
	svc	#0
1:	b	1b

_umsg:
	.ascii	"hello from user mode\n"
	.equ	_umsg_len, . - _umsg
_umsg2:
	.ascii	"fault-test: touching 0x0...\n"
	.equ	_umsg2_len, . - _umsg2

	.align 2
	.ltorg				/* literals for ldr = stay in the copy */

_uprog_end:
