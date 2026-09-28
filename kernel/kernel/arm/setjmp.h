/*
 * mach3/kernel/arm/setjmp.h -- ARM setjmp/longjmp for ddb.
 */
#ifndef	_MACHINE_SETJMP_H_
#define	_MACHINE_SETJMP_H_

#ifndef	ASSEMBLER
typedef unsigned int	jmp_buf[16];	/* r4-r11, sp, lr + pad */
typedef jmp_buf		jmp_buf_t;	/* the name ddb expects */

extern int	_setjmp(jmp_buf);
extern void	longjmp(jmp_buf, int);
#define	setjmp	_setjmp
#endif	/* ASSEMBLER */

#endif	/* _MACHINE_SETJMP_H_ */
