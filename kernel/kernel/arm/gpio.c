/*
 * mach3/kernel/arm/gpio.c -- Driver GPIO QaonicOS (Fase D).
 *
 * Port dari archive/kernel-scratch/kernel/src/gpio.c (terbukti di
 * kernel lama).  Dua backend:
 *  - BOARD_RV1103: register ASLI Rockchip RV1103:
 *      GPIO0 @ 0xff380000, GPIO1 @ 0xff530000, GPIO2 @ 0xff540000,
 *      GPIO3 @ 0xff550000, GPIO4 @ 0xff560000
 *    Layout per bank (Linux drivers/gpio/gpio-rockchip.c):
 *      SW_PORTx_DR  = base + port*8        (data)
 *      SW_PORTx_DDR = base + port*8 + 4     (0=input, 1=output)
 *      EXT_PORTx    = base + 0x50 + port*4  (baca level pin)
 *  - default (QEMU -M virt): register file di-RAM ("mock") dengan
 *    OFFSET YANG SAMA PERSIS seperti Rockchip, sehingga logika
 *    offset driver identik di kedua backend.  Alamat MMIO fisik
 *    TIDAK disentuh (unmapped di QEMU -> data abort).
 *
 * CATATAN RV1103: clock gate GPIO (CRU) harus dibuka sebelum
 * register disentuh (biasanya sudah oleh bootloader).  Backend
 * RV1103 wajib lolos compile saja di fase ini.
 */

#define	GPIO_BANK_COUNT		5u
#define	GPIO_PINS_PER_BANK	32u

/* --- offset register (Rockchip) --- */
#define	RK_DR_OFF(port)		((unsigned int)(port) * 8u)
#define	RK_DDR_OFF(port)	((unsigned int)(port) * 8u + 4u)
#define	RK_EXT_OFF(port)	(0x50u + (unsigned int)(port) * 4u)
#define	RK_BANK_SIZE		0x60u	/* cukup untuk EXT_PORTD */

#if defined(BOARD_RV1103)
/* Base MMIO asli per bank. */
static volatile unsigned int *const gpio_bases[GPIO_BANK_COUNT] = {
	(volatile unsigned int *)0xff380000u,
	(volatile unsigned int *)0xff530000u,
	(volatile unsigned int *)0xff540000u,
	(volatile unsigned int *)0xff550000u,
	(volatile unsigned int *)0xff560000u,
};
#else
/* QEMU: register file di RAM, offset identik Rockchip. */
static volatile unsigned int	gpio_virt_regs[GPIO_BANK_COUNT]
					 [RK_BANK_SIZE / 4u];
#endif

static volatile unsigned int *
gpio_base(unsigned int bank)
{
#if defined(BOARD_RV1103)
	return gpio_bases[bank];
#else
	return &gpio_virt_regs[bank][0];
#endif
}

void
gpio_init(void)
{
#if !defined(BOARD_RV1103)
	unsigned int b, i;
	/* BSS sudah nol; loop eksplisit agar deterministik bila
	 * dipanggil ulang. */
	for (b = 0u; b < GPIO_BANK_COUNT; b++)
		for (i = 0u; i < RK_BANK_SIZE / 4u; i++)
			gpio_virt_regs[b][i] = 0u;
#else
	/* Hardware asli: tidak disentuh (clock gate = urusan
	 * bootloader/CRU). */
#endif
}

int
gpio_set(unsigned int bank, unsigned int pin, unsigned int val)
{
	volatile unsigned int *base;
	volatile unsigned char *reg8;
	unsigned int port, bit;

	if (bank >= GPIO_BANK_COUNT || pin >= GPIO_PINS_PER_BANK)
		return -1;
	port = pin / 8u;
	bit = pin % 8u;
	base = gpio_base(bank);

	/* 1. Jadikan output: set bit di DDR. */
	reg8 = (volatile unsigned char *)base + RK_DDR_OFF(port);
	*reg8 = (unsigned char)(*reg8 | (unsigned char)(1u << bit));

	/* 2. Drive level: tulis bit di DR. */
	reg8 = (volatile unsigned char *)base + RK_DR_OFF(port);
	if (val != 0u)
		*reg8 = (unsigned char)(*reg8 | (unsigned char)(1u << bit));
	else
		*reg8 = (unsigned char)(*reg8 & (unsigned char)~(1u << bit));

#if !defined(BOARD_RV1103)
	/* Mock: EXT_PORT mencerminkan level yang di-drive (di hardware
	 * asli, baca EXT_PORT pin output = level output). */
	reg8 = (volatile unsigned char *)base + RK_EXT_OFF(port);
	if (val != 0u)
		*reg8 = (unsigned char)(*reg8 | (unsigned char)(1u << bit));
	else
		*reg8 = (unsigned char)(*reg8 & (unsigned char)~(1u << bit));
#endif
	return 0;
}

int
gpio_get(unsigned int bank, unsigned int pin)
{
	volatile unsigned int *base;
	volatile unsigned char *reg8;
	unsigned int port, bit;

	if (bank >= GPIO_BANK_COUNT || pin >= GPIO_PINS_PER_BANK)
		return -1;
	port = pin / 8u;
	bit = pin % 8u;
	base = gpio_base(bank);

	reg8 = (volatile unsigned char *)base + RK_EXT_OFF(port);
	return ((*reg8 >> bit) & 1u) ? 1 : 0;
}
