/*
 * kernel/kernel/arm/audio.c -- RV1103 ACODEC (audio codec) driver.
 *
 * D1: ACODEC init + register readback. D2/D3/D4 add I2S RX, DMA,
 * syscalls, and C6 uplink.
 *
 * Register offsets from sound/soc/codecs/rv1106_codec.h
 * (rockchip-linux kernel develop-5.10, supports rockchip,rv1103-codec).
 * WARNING: TRM RV1106 V0.3 analog register offsets do NOT match the
 * shipping driver; offsets below follow the driver.
 *
 * Follows the gpio.c pattern: on BOARD_RV1103, real MMIO at
 * 0xFF480000; on QEMU (no ACODEC model), a virtual register file in
 * RAM with identical offsets, so init/readback code paths are
 * testable. Silicon verification = H4 (hardware test plan).
 */

#include "machine_routines.h"	/* delay() */

/* --- ACODEC register map (byte offsets from 0xFF480000) --- */
#define	ACODEC_BASE		0xff480000u
#define	ACODEC_SIZE		0x1000u

#define	ACODEC_ADC_L_DIG_VOL	0x20u	/* REG 0x08 */
#define	ACODEC_ADC_R_DIG_VOL	0x24u	/* REG 0x09 */
#define	ACODEC_ADC_ANA_CTL0	0x88u	/* REG 0x22 */
#define	ACODEC_ADC_ANA_CTL1	0x8cu	/* REG 0x23 */
#define	ACODEC_ADC_ANA_CTL2	0x90u	/* REG 0x24: mic gain */
#define	ACODEC_ADC_ANA_CTL3	0x94u	/* REG 0x25 */
#define	ACODEC_ADC_ANA_CTL6	0xa0u	/* REG 0x28 */

/* --- ACODEC_ADC_ANA_CTL0 bits --- */
#define	ADC_IBIAS_MSK		(0x1u << 4)
#define	ADC_IBIAS_EN		(0x1u << 4)

/* --- ACODEC_ADC_ANA_CTL1 bits --- */
#define	ADC_L_MIC_MSK		(0x1u << 7)
#define	ADC_L_MIC_WORK		(0x1u << 7)
#define	ADC_L_REF_VOL_BUF_MSK	(0x1u << 5)
#define	ADC_L_REF_VOL_BUF_EN	(0x1u << 5)
#define	ADC_L_MIC_SIGNAL_MSK	(0x1u << 6)
#define	ADC_L_MIC_SIGNAL_WORK	(0x1u << 6)
#define	ADC_R_MIC_MSK		(0x1u << 3)
#define	ADC_R_MIC_WORK		(0x1u << 3)
#define	ADC_R_REF_VOL_BUF_MSK	(0x1u << 1)
#define	ADC_R_REF_VOL_BUF_EN	(0x1u << 1)
#define	ADC_R_MIC_SIGNAL_MSK	(0x1u << 2)
#define	ADC_R_MIC_SIGNAL_WORK	(0x1u << 2)

/* --- ACODEC_ADC_ANA_CTL2 bits (mic gain) --- */
#define	ADC_L_MIC_GAIN_SFT	6u
#define	ADC_L_MIC_GAIN_MSK	(0x3u << ADC_L_MIC_GAIN_SFT)
#define	ADC_L_MIC_GAIN_20DB	(0x2u << ADC_L_MIC_GAIN_SFT)
#define	ADC_R_MIC_GAIN_SFT	4u
#define	ADC_R_MIC_GAIN_MSK	(0x3u << ADC_R_MIC_GAIN_SFT)
#define	ADC_R_MIC_GAIN_20DB	(0x2u << ADC_R_MIC_GAIN_SFT)
/* DTS init-mic-gain = <0x22>: L 20dB + R 20dB. */

/* --- ACODEC_ADC_ANA_CTL3 bits --- */
#define	ADC_L_MSK		(0x1u << 5)
#define	ADC_L_EN		(0x1u << 5)
#define	MIC_L_MSK		(0x1u << 4)
#define	MIC_L_EN		(0x1u << 4)
#define	ADC_R_MSK		(0x1u << 1)
#define	ADC_R_EN		(0x1u << 1)
#define	MIC_R_MSK		(0x1u << 0)
#define	MIC_R_EN		(0x1u << 0)

/* --- ACODEC_ADC_ANA_CTL6 bits --- */
#define	ADC_L_CLK_MSK		(0x1u << 6)
#define	ADC_L_CLK_WORK		(0x1u << 6)
#define	ADC_L_WORK		(0x1u << 5)
#define	ADC_L_SIGNAL_EN		(0x1u << 4)
#define	ADC_R_CLK_MSK		(0x1u << 2)
#define	ADC_R_CLK_WORK		(0x1u << 2)
#define	ADC_R_WORK		(0x1u << 1)
#define	ADC_R_SIGNAL_EN		(0x1u << 0)
#define	ADC_L_ALC_MSK		(0x1u << 7)
#define	ADC_L_ALC_WORK		(0x1u << 7)
#define	ADC_R_ALC_MSK		(0x1u << 3)
#define	ADC_R_ALC_WORK		(0x1u << 3)

/* --- GRF audio route (RV1103B, from rv1106_codec.c) --- */
#define	SYS_GRF_BASE		0xff000000u
#define	SYS_GRF_SIZE		0x68000u
#define	GRF_AUDIO_CON0		0x50020u
#define	GRF_AUDIO_CON1		0x50024u
#define	AUDIO_CON0_MUX_MSK	((0x3u << 19) | (0x3u << 21) | \
				 (0x3u << 23) | (0x3u << 25))
#define	AUDIO_CON0_MUX_SEL_ACODEC \
				((0x3u << 3) | (0x3u << 5) | \
				 (0x3u << 7) | (0x3u << 9))
#define	AUDIO_CON1_MUX_MSK	((0x7u << 16) | (0x3u << 21))
#define	AUDIO_CON1_IO_SEL_DIS	0x00000000u	/* SDO/SCLKLRCLK -> GND */

/* --- Register access --- */
#if defined(BOARD_RV1103)
static volatile unsigned int *const acodec_hw =
    (volatile unsigned int *)ACODEC_BASE;
static volatile unsigned int *const grf_hw =
    (volatile unsigned int *)SYS_GRF_BASE;
#else
/* QEMU: register files in RAM, identical offsets. */
static volatile unsigned int acodec_virt_regs[ACODEC_SIZE / 4u];
static volatile unsigned int grf_virt_regs[SYS_GRF_SIZE / 4u];
#endif

static volatile unsigned int *
acodec_base(void)
{
#if defined(BOARD_RV1103)
	return acodec_hw;
#else
	return &acodec_virt_regs[0];
#endif
}

static volatile unsigned int *
grf_base(void)
{
#if defined(BOARD_RV1103)
	return grf_hw;
#else
	return &grf_virt_regs[0];
#endif
}

static unsigned int
acodec_read(unsigned int off)
{
	if (off >= ACODEC_SIZE)
		return 0u;
	return acodec_base()[off / 4u];
}

static void
acodec_write(unsigned int off, unsigned int val)
{
	if (off >= ACODEC_SIZE)
		return;
	acodec_base()[off / 4u] = val;
}

static void
acodec_update_bits(unsigned int off, unsigned int mask, unsigned int val)
{
	unsigned int r;

	r = acodec_read(off);
	r = (r & ~mask) | (val & mask);
	acodec_write(off, r);
}

/*
 * GRF write with HIWORD mask (Rockchip convention):
 * upper 16 bits = write mask, lower 16 bits = value.
 */
static void
grf_hiword_write(unsigned int off, unsigned int mask, unsigned int val)
{
	volatile unsigned int *b;
	unsigned int idx;

	if (off >= SYS_GRF_SIZE)
		return;
	idx = off / 4u;
	b = grf_base();
#if defined(BOARD_RV1103)
	b[idx] = (mask << 16) | (val & 0xffffu);
#else
	/* Virtual: emulate mask semantics. */
	b[idx] = (b[idx] & ~mask) | (val & mask);
#endif
}

/*
 * Clock + reset for ACODEC.
 *
 * From clk-rv1106.c: PCLK_ACODEC gate = PERICLKGATE_CON(6) bit 3;
 * MCLK_ACODEC_TX gate = PERICLKGATE_CON(6) bit 4, div =
 * PERICLKSEL_CON(8) bits 0-7; reset = SRST_P_ACODEC (CRU base
 * 0xFF3A0000).
 *
 * TODO(H4): absolute CRU offsets for PERICLKGATE_CON need the TRM
 * CRU chapter. Assumption for now: the bootloader leaves these
 * clocks enabled (standard when the codec driver is expected).
 * On real hardware, verify with a register dump before trusting
 * capture.
 */
static void
acodec_clock_enable(void)
{
#if defined(BOARD_RV1103)
	/* TODO: write PERICLKGATE_CON(6) bits 3,4 once the absolute
	 * offset is confirmed from the TRM. */
	printf("audio: CRU gate TODO (assuming bootloader-enabled)\n");
#else
	/* QEMU: nothing to gate. */
#endif
}

/*
 * acodec_init: enable ADC capture path (vendor steps 00-10 from
 * rv1106_codec_adc_enable(), both L+R; mono selection happens at
 * the I2S/DMA stage in D2).
 *
 * Returns 0 on success.
 */
int
acodec_init(void)
{
	/* Step 1: GRF route -- capture from ACODEC mic (RV1103B). */
	grf_hiword_write(GRF_AUDIO_CON0,
	    AUDIO_CON0_MUX_MSK, AUDIO_CON0_MUX_SEL_ACODEC);
	grf_hiword_write(GRF_AUDIO_CON1,
	    AUDIO_CON1_MUX_MSK, AUDIO_CON1_IO_SEL_DIS);

	/* Step 2: clocks + reset. */
	acodec_clock_enable();

	/* Vendor step 01: mic L/R work. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL1,
	    ADC_L_MIC_MSK | ADC_R_MIC_MSK,
	    ADC_L_MIC_WORK | ADC_R_MIC_WORK);

	/* Vendor step 02: ADC ibias enable. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL0,
	    ADC_IBIAS_MSK, ADC_IBIAS_EN);

	/* Vendor step 03: ref-vol buffer L/R enable; wait VREF stable. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL1,
	    ADC_L_REF_VOL_BUF_MSK | ADC_R_REF_VOL_BUF_MSK,
	    ADC_L_REF_VOL_BUF_EN | ADC_R_REF_VOL_BUF_EN);
	delay(100000);		/* 100 ms */

	/* Vendor step 04: mic L/R enable. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL3,
	    MIC_L_MSK | MIC_R_MSK,
	    MIC_L_EN | MIC_R_EN);

	/* Vendor step 05: ADC L/R enable. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL3,
	    ADC_L_MSK | ADC_R_MSK,
	    ADC_L_EN | ADC_R_EN);

	/* Vendor step 06: ADC clock L/R work. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL6,
	    ADC_L_CLK_MSK | ADC_R_CLK_MSK,
	    ADC_L_CLK_WORK | ADC_R_CLK_WORK);

	/* Vendor step 07: ADC L/R work. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL6,
	    ADC_L_WORK | ADC_R_WORK,
	    ADC_L_WORK | ADC_R_WORK);

	/* Vendor step 08: signal path L/R enable. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL6,
	    ADC_L_SIGNAL_EN | ADC_R_SIGNAL_EN,
	    ADC_L_SIGNAL_EN | ADC_R_SIGNAL_EN);

	/* Vendor step 09: ALC L/R work (driver default). */
	acodec_update_bits(ACODEC_ADC_ANA_CTL6,
	    ADC_L_ALC_MSK | ADC_R_ALC_MSK,
	    ADC_L_ALC_WORK | ADC_R_ALC_WORK);

	/* Vendor step 10: mic signal L/R work. */
	acodec_update_bits(ACODEC_ADC_ANA_CTL1,
	    ADC_L_MIC_SIGNAL_MSK | ADC_R_MIC_SIGNAL_MSK,
	    ADC_L_MIC_SIGNAL_WORK | ADC_R_MIC_SIGNAL_WORK);

	/* Vendor step 11: mic gain L/R = 20dB (DTS init-mic-gain 0x22). */
	acodec_update_bits(ACODEC_ADC_ANA_CTL2,
	    ADC_L_MIC_GAIN_MSK | ADC_R_MIC_GAIN_MSK,
	    ADC_L_MIC_GAIN_20DB | ADC_R_MIC_GAIN_20DB);

	return 0;
}

/*
 * acodec_reg_read: for diagnostics / D1 readback test.
 * Returns 0 for out-of-range offsets.
 */
unsigned int
acodec_reg_read(unsigned int off)
{
	return acodec_read(off);
}

void
audio_init(void)
{
#if !defined(BOARD_RV1103)
	unsigned int i;

	/* BSS already zero; explicit loop for determinism on re-init. */
	for (i = 0u; i < ACODEC_SIZE / 4u; i++)
		acodec_virt_regs[i] = 0u;
	for (i = 0u; i < SYS_GRF_SIZE / 4u; i++)
		grf_virt_regs[i] = 0u;
#else
	/* Real hardware: registers keep bootloader state. */
#endif
}
