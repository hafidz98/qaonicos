/* mach3/kernel/arm/asm.h -- ARM assembler macros. */
#ifndef	_MACHINE_ASM_H_
#define	_MACHINE_ASM_H_

#define	LEAF(name)			\
	.text;				\
	.align 2;			\
	.globl name;			\
	.type name, %function;		\
name:

#define	END(name)			\
	.size name, .-name

#define	EXPORT(name)			\
	.globl name;			\
name:

#endif	/* _MACHINE_ASM_H_ */
