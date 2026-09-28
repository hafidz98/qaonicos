/*
 * mach3/kernel/arm/pmap.h -- ARM pmap interface.
 * Identity-mapped kernel (VA == PA); single L1 shared by kernel_pmap.
 * Modelled on CMU Mach 3.0 mips/pmap.h.
 */
#ifndef	_PMAP_MACHINE_
#define	_PMAP_MACHINE_	1

/*
 * M3 ARM: MD pmap module implements pmap_steal_memory/pmap_startup
 * itself (single steal cursor); MI generic versions in vm_resident.c
 * are not compiled.  (Without this, clang can inline the MI generic
 * pmap_steal_memory into vm_page_bootstrap while other TUs call the
 * MD override -- two independent cursors, overlapping allocations.)
 */
#define	MACHINE_PAGES	1

#ifndef	ASSEMBLER
#include <mach/boolean.h>
#include <machine/pte.h>
#include <kern/zalloc.h>
#include <kern/lock.h>
#include <mach/machine/vm_param.h>
#include <mach/vm_statistics.h>

struct pmap {
	l1_entry_t	*l1;		/* L1 table (kernel: shared boot L1) */
	int		ref_count;
	decl_simple_lock_data(,lock)
	struct pmap_statistics stats;
};

typedef struct pmap	*pmap_t;
#define	PMAP_NULL	((pmap_t) 0)

extern pmap_t	kernel_pmap;

#ifdef	KERNEL
/* UP, single address space: activating kernel pmap = ensure TTBR0. */
#define	PMAP_ACTIVATE_KERNEL(cpu)	arm_pmap_activate_kernel()
#define	PMAP_DEACTIVATE_KERNEL(cpu)
#define	PMAP_ACTIVATE_USER(pmap, th, cpu)	arm_pmap_activate_user(pmap)
#define	PMAP_DEACTIVATE_USER(pmap, th, cpu)
#define	PMAP_ACTIVATE(pmap, th, cpu)	arm_pmap_activate(pmap)
#define	PMAP_DEACTIVATE(pmap, th, cpu)

extern void	arm_pmap_activate_kernel(void);
extern void	arm_pmap_activate_user(pmap_t);
extern void	arm_pmap_activate(pmap_t);

/* physical <-> virtual (identity) */
#define	pmap_phys_to_frame(pa)		arm_btop(pa)
#define	PMAP_PHYS_TO_FRAME(pa)		arm_btop(pa)

#endif	/* KERNEL */
#endif	/* !ASSEMBLER */

#define	VA_PAGEOFF	12
#define	VA_PAGEMASK	0xfffff000
#define	VA_OFFMASK	0x00000fff

#endif	/* _PMAP_MACHINE_ */
