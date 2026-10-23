/*
 * mach3/kernel/arm/ast.h -- ARM asynchronous system traps.
 * AST pending flag lives in the pcb; checked on trap return.
 */
#ifndef	_MACHINE_AST_H_
#define	_MACHINE_AST_H_

#include <machine/ast_types.h>

#define	AST_NONE	0
#define	AST_PREEMPT	1
#define	AST_TERMINATE	2

#endif	/* _MACHINE_AST_H_ */
