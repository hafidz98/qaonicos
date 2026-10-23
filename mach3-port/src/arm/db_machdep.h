/*
 * mach3/kernel/arm/db_machdep.h -- ARM ddb machine-dependent definitions.
 * Modelled on CMU Mach 3.0 mips/db_machdep.h.
 */
#ifndef	_ARM_DB_MACHDEP_H_
#define	_ARM_DB_MACHDEP_H_
#include <mach/machine/vm_types.h>
#include <mach/machine/vm_param.h>
#include <machine/thread.h>		/* for thread_status */
#include <mach/boolean.h>
typedef	vm_offset_t	db_addr_t;	/* address - unsigned */
typedef	int		db_expr_t;	/* expression - signed */
typedef struct arm_saved_state db_regs_t;
extern db_regs_t	*db_cur_exc_frame;	/* register state */
#define	DDB_REGS	db_cur_exc_frame
#define	PC_REGS(regs)	((db_addr_t)(regs)->pc)
#define	BKPT_INST	0xe1200070	/* ARM bkpt #0 */
#define	BKPT_SIZE	(4)		/* size of breakpoint inst */
#define	BKPT_SET(inst)	(BKPT_INST)
#define	IS_BREAKPOINT_TRAP(type, code)	((type) == 7)	/* EXC_BPT */
#define	IS_WATCHPOINT_TRAP(type, code)	(0)
#define	SOFTWARE_SSTEP			1	/* no hardware support */
#define	next_instr_address(v,b,task)	((db_addr_t)(v)+4)
#define	inst_trap_return(ins)		(((ins) & 0x0ffffff0) == 0xe1200070)
#define	inst_return(ins)		(((ins) & 0x0e000000) == 0x0a000000)
#define	inst_call(ins)			(((ins) & 0x0f000000) == 0x0b000000)
#define	inst_branch(ins)		(((ins) & 0x0e000000) == 0x0a000000)
#define	inst_load(ins)			(0)
#define	inst_store(ins)			(0)
#define	DB_ACCESS_LEVEL		2	/* access any space */
#define DB_CHECK_ACCESS(addr,size,task) \
			db_check_access(addr,size,task)
#define DB_PHYS_EQ(task1,addr1,task2,addr2) \
			db_phys_eq(task1,addr1,task2,addr2)
#define DB_VALID_KERN_ADDR(addr) \
			((addr) >= VM_MIN_KERNEL_ADDRESS && \
			 (addr) < VM_MAX_KERNEL_ADDRESS)
#define DB_VALID_ADDRESS(addr,user) \
			((!(user) && DB_VALID_KERN_ADDR(addr)) || \
			 ((user) && (addr) < VM_MAX_ADDRESS))
boolean_t	db_check_access(/* vm_offset_t, int, task_t */);
boolean_t	db_phys_eq(/* task_t, vm_offset_t, task_t, vm_offset_t */);
#define DB_TASK_NAME(task) \
		db_task_name(task)
#define DB_TASK_NAME_TITLE	"COMMAND                "
#endif	/* _ARM_DB_MACHDEP_H_ */
