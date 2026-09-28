/*
 * mach3/kernel/arm/pte.h -- ARMv7 short-descriptor page table entries.
 */
#ifndef	_ARM_PTE_H_
#define	_ARM_PTE_H_

/* L1 descriptor types */
#define	L1_TYPE_MASK		0x3
#define	L1_TYPE_FAULT		0x0
#define	L1_TYPE_TABLE		0x1	/* page table (PXN/NG bits in v7) */
#define	L1_TYPE_SECTION		0x2
#define	L1_TYPE_SUPERSECTION	0x2	/* with bit 18 set */

/* L1 section descriptor bits */
#define	L1_SEC_B		(1<<2)
#define	L1_SEC_C		(1<<3)
#define	L1_SEC_XN		(1<<4)
#define	L1_SEC_DOMAIN_SHIFT	5
#define	L1_SEC_DOMAIN(x)	((x)<<L1_SEC_DOMAIN_SHIFT)
#define	L1_SEC_AP_SHIFT		10
#define	L1_SEC_AP(x)		((x)<<L1_SEC_AP_SHIFT)
#define	L1_SEC_TEX_SHIFT	12
#define	L1_SEC_AP2		(1<<15)
#define	L1_SEC_S		(1<<16)
#define	L1_SEC_nG		(1<<17)
#define	L1_SEC_SUPER		(1<<18)
#define	L1_SEC_NS		(1<<19)

/* AP values (AP[2:1] + AP[0]): full access */
#define	AP_KRW_URW		0x3	/* kernel RW, user RW */
#define	AP_KRW_URO		0x2	/* kernel RW, user RO */
#define	AP_KRW_UNA		0x1	/* kernel RW, user none */
#define	AP_KRO_UNA		0x5	/* kernel RO (+AP2), user none */

/* L2 descriptor types */
#define	L2_TYPE_MASK		0x3
#define	L2_TYPE_FAULT		0x0
#define	L2_TYPE_LARGE		0x1
#define	L2_TYPE_SMALL		0x2

/* L2 small page bits */
#define	L2_SP_B			(1<<2)
#define	L2_SP_C			(1<<3)
#define	L2_SP_AP_SHIFT		4
#define	L2_SP_AP(x)		((x)<<L2_SP_AP_SHIFT)
#define	L2_SP_TEX_SHIFT		6
#define	L2_SP_AP2		(1<<9)
#define	L2_SP_S			(1<<10)
#define	L2_SP_nG		(1<<11)
#define	L2_SP_XN		(1<<0)	/* NOT USED: would corrupt L1_TYPE bits[1:0];
					   ARMv7 short-desc small pages have no XN bit */

#define	ARM_L1_TABLE_ENTRIES	4096
#define	ARM_L1_TABLE_SIZE	(ARM_L1_TABLE_ENTRIES*4)
#define	ARM_L2_TABLE_ENTRIES	256
#define	ARM_L2_TABLE_SIZE	(ARM_L2_TABLE_ENTRIES*4)
#define	ARM_SECTION_SIZE	0x100000

typedef unsigned int	l1_entry_t;
typedef unsigned int	l2_entry_t;

#endif	/* _ARM_PTE_H_ */
