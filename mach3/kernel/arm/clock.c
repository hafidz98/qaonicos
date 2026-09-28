/*
 * mach3/kernel/arm/clock.c -- ARM generic timer (virtual) clock.
 *
 * 100 Hz periodic interrupt via the virtual timer PPI (30).
 * MI entry point for ticks: clock_interrupt() (kern/mach_clock.c),
 * called from trap.c.
 */
#include <mach/machine/vm_types.h>
#include <machine/mach_param.h>		/* HZ */
#include <sys/time.h>

extern void	gic_enable_irq(unsigned int);
extern void	gic_init(void);

#define	ARM_VTIMER_PPI	30

static unsigned int	timer_freq;	/* CNTFRQ */
static unsigned int	timer_tval;	/* ticks per HZ */

static unsigned int
read_cntfrq(void)
{
	unsigned int v;
	__asm__ volatile ("mrc p15, 0, %0, c14, c0, 0" : "=r" (v));
	return v;
}

static unsigned long long
read_cntvct(void)
{
	unsigned long long v;
	__asm__ volatile ("mrrc p15, 1, %Q0, %R0, c14" : "=r" (v));
	return v;
}

static void
write_cntv_tval(unsigned int v)
{
	__asm__ volatile ("mcr p15, 0, %0, c14, c3, 0" : : "r" (v));
}

static void
write_cntv_ctl(unsigned int v)
{
	__asm__ volatile ("mcr p15, 0, %0, c14, c3, 1" : : "r" (v));
}

/*
 * startrtclock: start the periodic 100Hz timer.
 * Called from cpu_launch_first_thread() (MI) once a thread is active.
 */
void
startrtclock(void)
{
	timer_freq = read_cntfrq();
	if (timer_freq == 0)
		timer_freq = 62500000u;	/* QEMU virt default */
	timer_tval = timer_freq / HZ;

	gic_init();
	gic_enable_irq(ARM_VTIMER_PPI);

	write_cntv_tval(timer_tval);
	write_cntv_ctl(0x1u);		/* enable, unmasked */
}

/*
 * arm_timer_eoi: re-arm the timer (called from trap.c before GIC EOI).
 */
void
arm_timer_eoi(void)
{
	write_cntv_tval(timer_tval);
}

int
sched_usec_elapsed(void)
{
	/* M3: 10ms per 100Hz tick (SIMPLE_CLOCK drift compensation). */
	return 10000;
}

void
setsoftclock(void)
{
	/* M3: no software clock interrupt; hardclock runs at IPL. */
}

void
inittodr(void)
{
}

void
resettodr(void)
{
}

/*
 * microtime: time since boot (no RTC in M3).
 */
void
microtime(struct timeval *tv)
{
	unsigned long long ticks = read_cntvct();
	unsigned long long usec;

	if (timer_freq == 0)
		timer_freq = 62500000u;
	usec = (ticks * 1000000ull) / timer_freq;
	tv->tv_sec = usec / 1000000ull;
	tv->tv_usec = usec % 1000000ull;
}

/*
 * delay: busy-wait ~usec microseconds.
 */
void
delay(int usec)
{
	unsigned long long start = read_cntvct();
	unsigned long long delta;

	if (timer_freq == 0)
		timer_freq = 62500000u;
	delta = ((unsigned long long)usec * timer_freq) / 1000000ull;
	while (read_cntvct() - start < delta)
		;
}
