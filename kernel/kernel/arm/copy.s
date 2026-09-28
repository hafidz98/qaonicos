/*
 * mach3/kernel/arm/copy.s -- ARM block copy primitives.
 *
 * Identity-mapped: copyin/copyout are just bcopy (no user/kernel
 * distinction in M3; no user tasks yet).
 */
	.text
	.align 2

/* void bcopy(const void *src, void *dst, unsigned int n) */
	.globl bcopy
	.type bcopy, %function
bcopy:
	push	{r4-r11, lr}
	/* r0=src, r1=dst, r2=n */
	cmp	r2, #0
	beq	.Lbcopy_done
	/* word-align if possible */
.Lbcopy_loop:
	cmp	r2, #4
	blt	.Lbcopy_tail
	ldr	r3, [r0], #4
	str	r3, [r1], #4
	sub	r2, r2, #4
	b	.Lbcopy_loop
.Lbcopy_tail:
	cmp	r2, #0
	beq	.Lbcopy_done
	ldrb	r3, [r0], #1
	strb	r3, [r1], #1
	sub	r2, r2, #1
	b	.Lbcopy_tail
.Lbcopy_done:
	pop	{r4-r11, pc}
	.size bcopy, .-bcopy

/* void bzero(void *dst, unsigned int n) */
	.globl bzero
	.type bzero, %function
bzero:
	push	{r4-r11, lr}
	/* r0=dst, r1=n */
	mov	r2, #0
	cmp	r1, #0
	beq	.Lbzero_done
.Lbzero_loop:
	cmp	r1, #4
	blt	.Lbzero_tail
	str	r2, [r0], #4
	sub	r1, r1, #4
	b	.Lbzero_loop
.Lbzero_tail:
	cmp	r1, #0
	beq	.Lbzero_done
	strb	r2, [r0], #1
	sub	r1, r1, #1
	b	.Lbzero_tail
.Lbzero_done:
	pop	{r4-r11, pc}
	.size bzero, .-bzero

/* void ovbcopy(const void *src, void *dst, unsigned int n) -- overlap safe */
	.globl ovbcopy
	.type ovbcopy, %function
ovbcopy:
	push	{r4-r11, lr}
	/* r0=src, r1=dst, r2=n; copy backwards if dst > src */
	cmp	r1, r0
	bls	.Lov_fwd
	/* backwards */
	add	r0, r0, r2
	add	r1, r1, r2
.Lov_back:
	cmp	r2, #0
	beq	.Lov_done
	ldrb	r3, [r0, #-1]!
	strb	r3, [r1, #-1]!
	sub	r2, r2, #1
	b	.Lov_back
.Lov_fwd:
	bl	bcopy_internal
.Lov_done:
	pop	{r4-r11, pc}
	.size ovbcopy, .-ovbcopy

bcopy_internal:
	cmp	r2, #0
	bxeq	lr
.Lbi_loop:
	cmp	r2, #4
	blt	.Lbi_tail
	ldr	r3, [r0], #4
	str	r3, [r1], #4
	sub	r2, r2, #4
	b	.Lbi_loop
.Lbi_tail:
	cmp	r2, #0
	bxeq	lr
	ldrb	r3, [r0], #1
	strb	r3, [r1], #1
	sub	r2, r2, #1
	b	.Lbi_tail

/* int copyin(const void *usrc, void *kdst, unsigned int n) */
	.globl copyin
	.type copyin, %function
copyin:
	push	{r4-r11, lr}
	bl	bcopy_internal
	mov	r0, #0			/* success */
	pop	{r4-r11, pc}
	.size copyin, .-copyin

/* int copyout(const void *ksrc, void *udst, unsigned int n) */
	.globl copyout
	.type copyout, %function
copyout:
	push	{r4-r11, lr}
	bl	bcopy_internal
	mov	r0, #0
	pop	{r4-r11, pc}
	.size copyout, .-copyout

/* int copyinstr(const void *usrc, void *kdst, unsigned int max, unsigned int *n) */
	.globl copyinstr
	.type copyinstr, %function
copyinstr:
	push	{r4-r11, lr}
	/* r0=usrc, r1=kdst, r2=max, r3=n */
	mov	r4, #0			/* count */
.Lci_loop:
	cmp	r4, r2
	bhs	.Lci_fail		/* ENAMETOOLONG */
	ldrb	r5, [r0], #1
	strb	r5, [r1], #1
	add	r4, r4, #1
	cmp	r5, #0
	bne	.Lci_loop
	cmp	r3, #0
	strne	r4, [r3]
	mov	r0, #0
	pop	{r4-r11, pc}
.Lci_fail:
	mov	r0, #63			/* ENAMETOOLONG */
	pop	{r4-r11, pc}
	.size copyinstr, .-copyinstr

/* int copyoutstr(const void *ksrc, void *udst, unsigned int max, unsigned int *n) */
	.globl copyoutstr
	.type copyoutstr, %function
copyoutstr:
	b	copyinstr
	.size copyoutstr, .-copyoutstr

/*
 * copyinmsg(from, to, len): copy a Mach message.  Returns 0 on success.
 * copyoutmsg(from, to, len): copy a Mach message.  Returns 0 on success.
 * Identity-mapped: plain word copy (like mips mips_copyin.s).
 */
	.global	copyinmsg
	.type	copyinmsg, %function
copyinmsg:
	cmp	r0, #0
	beq	copyinmsg_err
	add	r3, r0, r2
	cmp	r3, #0
	beq	copyinmsg_err
1:	ldr	r3, [r0], #4
	str	r3, [r1], #4
	subs	r2, r2, #4
	bne	1b
	mov	r0, #0
	bx	lr
copyinmsg_err:
	mov	r0, #1
	bx	lr
	.size	copyinmsg, .-copyinmsg

	.global	copyoutmsg
	.type	copyoutmsg, %function
copyoutmsg:
	cmp	r1, #0
	beq	copyoutmsg_err
	add	r3, r1, r2
	cmp	r3, #0
	beq	copyoutmsg_err
1:	ldr	r3, [r0], #4
	str	r3, [r1], #4
	subs	r2, r2, #4
	bne	1b
	mov	r0, #0
	bx	lr
copyoutmsg_err:
	mov	r0, #1
	bx	lr
	.size	copyoutmsg, .-copyoutmsg
