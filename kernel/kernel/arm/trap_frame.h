/*
 * mach3/kernel/arm/trap_frame.h -- trap frame layout (M4 item 4).
 *
 * Built by locore.s exception handlers on the SVC stack via SRS:
 *   srsdb sp!, #0x13  ->  [sp] = LR, [sp+4] = SPSR  (then push {r0-r12})
 * Shared between trap.c (dispatch) and user.c (syscall/fault handling).
 */
#ifndef	_ARM_TRAP_FRAME_H_
#define	_ARM_TRAP_FRAME_H_

struct arm_trap_frame {
	unsigned int	r[13];	/* r0-r12 */
	unsigned int	svc_lr;	/* SVC-mode lr saat trap (di-push agar tak
				 * dirusak "bl arm_trap_handler"; Fase D fix) */
	unsigned int	lr;	/* adjusted return address */
	unsigned int	spsr;
};

#define	ARM_MODE_USR	0x10u
#define	ARM_MODE_SVC	0x13u

#endif	/* _ARM_TRAP_FRAME_H_ */
