/*
 * mach3/kernel/arm/gic.c -- ARM GIC-400 (GICv2) driver, QEMU -M virt.
 *
 * GICD @ 0x08000000, GICC @ 0x08010000.
 * Timer PPI (interrupt 30, virtual timer) is the only source used in M3.
 */
#define	GICD_BASE	0x08000000u
#define	GICC_BASE	0x08010000u

/* Distributor registers */
#define	GICD_CTLR	0x000u
#define	GICD_ISENABLER(n) (0x100u + ((n) << 2))
#define	GICD_IPRIORITYR(n) (0x400u + (n))

/* CPU interface registers */
#define	GICC_CTLR	0x00u
#define	GICC_PMR	0x04u
#define	GICC_IAR	0x0cu
#define	GICC_EOIR	0x10u

#define	GICD_REG(off)	(*(volatile unsigned int *)(GICD_BASE + (off)))
#define	GICC_REG(off)	(*(volatile unsigned int *)(GICC_BASE + (off)))

#define	ARM_VTIMER_PPI	30	/* virtual timer PPI */

void
gic_init(void)
{
	int i;

	/* Disable distributor while configuring. */
	GICD_REG(GICD_CTLR) = 0;

	/* Lowest priority for all 1020 interrupts. */
	for (i = 0; i < 255; i++)
		GICD_REG(GICD_IPRIORITYR(i)) = 0xa0a0a0a0u;

	/* Enable distributor (Group 0). */
	GICD_REG(GICD_CTLR) = 1;

	/* CPU interface: accept all priorities, enable Group 0. */
	GICC_REG(GICC_PMR) = 0xffu;
	GICC_REG(GICC_CTLR) = 1;
}

void
gic_enable_irq(unsigned int irq)
{
	GICD_REG(GICD_ISENABLER(irq >> 5)) = 1u << (irq & 31);
}

unsigned int
gic_get_irq(void)
{
	return GICC_REG(GICC_IAR) & 0x3ffu;
}

void
gic_eoi(unsigned int irq)
{
	GICC_REG(GICC_EOIR) = irq;
}
