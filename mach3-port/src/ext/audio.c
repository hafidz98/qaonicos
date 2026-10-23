/*
 * mach3-port/src/ext/audio.c -- RV1103 ACODEC (audio codec) driver.
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

/* --- I2S0_8CH (D2) --- */
#define	I2S_BASE		0xffae0000u
#define	I2S_SIZE		0x1000u
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
static volatile unsigned int i2s_virt_regs[I2S_SIZE / 4u];
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
	for (i = 0u; i < I2S_SIZE / 4u; i++)
		i2s_virt_regs[i] = 0u;
#else
	/* Real hardware: registers keep bootloader state. */
#endif
}

/* ============================================================
 * D2: I2S RX + PIO drain + ring buffer + SYS_AUDIO_READ.
 *
 * I2S0_8CH @ 0xFFAE0000 (rockchip,rv1106-i2s-tdm).
 * Register offsets from sound/soc/rockchip/rockchip_i2s_tdm.h.
 *
 * D2 uses PIO (poll RXFIFOLR, read RXDR). DMA via PL330 ch 21
 * is a follow-up (D2b): needs PL330 microcode.
 * ============================================================ */

#define	I2S_RXCR		0x0004u
#define	I2S_CKR			0x0008u
#define	I2S_DMACR		0x0010u
#define	I2S_XFER		0x001cu
#define	I2S_CLR			0x0020u
#define	I2S_RXDR		0x0028u
#define	I2S_RXFIFOLR		0x002cu

/* RXCR bits (from rockchip_i2s_tdm.h). */
#define	RXCR_VDW_SHIFT		0u
#define	RXCR_VDW(x)		(((x) - 1u) << RXCR_VDW_SHIFT)
#define	RXCR_TFS_SHIFT		5u
#define	RXCR_TFS_I2S		(0u << RXCR_TFS_SHIFT)
#define	RXCR_CSR_SHIFT		15u
#define	RXCR_CSR(x)		((x) << RXCR_CSR_SHIFT)	/* ch = x+1 */
#define	RXCR_SJM_SHIFT		12u
#define	RXCR_SJM_R		(0u << RXCR_SJM_SHIFT)	/* right-justified */

/* XFER bits. */
#define	XFER_RX_EN		(0x1u << 0)
#define	XFER_RX_DIS		(0x0u << 0)

/* CLR bits. */
#define	CLR_RXC			(0x1u << 0)	/* clear RX FIFO */

/* RXFIFOLR: bits [5:0] = RX FIFO level (0-32). */
#define	RXFIFOLR_RFL_MASK	0x3fu

/* CKR: TODO(H4) -- divider needs actual MCLK from CRU.
 * Placeholder: assume MCLK = 12.288 MHz, BCLK = 512 kHz
 * (16 kHz * 32) -> div 24. TRCM=RXONLY, master mode. */
#define	CKR_TRCM_SHIFT		28u
#define	CKR_TRCM_RXONLY		(2u << CKR_TRCM_SHIFT)
#define	CKR_MSS_SHIFT		27u
#define	CKR_MSS_MASTER		(0u << CKR_MSS_SHIFT)

#if defined(BOARD_RV1103)
static volatile unsigned int *const i2s_hw =
    (volatile unsigned int *)I2S_BASE;
#else
/* QEMU test hook: virtual RXDR returns incrementing pattern. */
static unsigned int i2s_virt_rxctr;
#endif

/* Forward: defined with ring buffer below. */
static int audio_running;

static volatile unsigned int *
i2s_base(void)
{
#if defined(BOARD_RV1103)
	return i2s_hw;
#else
	return &i2s_virt_regs[0];
#endif
}

static unsigned int
i2s_read(unsigned int off)
{
	if (off >= I2S_SIZE)
		return 0u;
#if !defined(BOARD_RV1103)
	if (off == I2S_RXDR) {
		/* Simulate streaming: 16-bit counter pattern. */
		return (i2s_virt_rxctr++ & 0xffffu);
	}
	if (off == I2S_RXFIFOLR) {
		/* Simulate: FIFO always has data when running. */
		return audio_running ? 16u : 0u;
	}
#endif
	return i2s_base()[off / 4u];
}

static void
i2s_write(unsigned int off, unsigned int val)
{
	if (off >= I2S_SIZE)
		return;
	i2s_base()[off / 4u] = val;
}

/* --- Ring buffer 8 KB (pattern TCP RX Q2a) --- */
#define	AUDIO_RING_SIZE		8192u
static unsigned char audio_ring[AUDIO_RING_SIZE];
static unsigned int audio_ring_head;	/* write pos (DMA/PIO) */
static unsigned int audio_ring_tail;	/* read pos (syscall) */
static unsigned int audio_ring_count;
static unsigned int audio_overrun;	/* D3: expose via syscall */
/* audio_running defined above (forward decl). */

static void
audio_ring_push(unsigned short sample)
{
	if (audio_ring_count + 2u > AUDIO_RING_SIZE) {
		/* Overrun: drop oldest sample. */
		audio_ring_tail = (audio_ring_tail + 2u) % AUDIO_RING_SIZE;
		audio_ring_count -= 2u;
		audio_overrun++;
	}
	audio_ring[audio_ring_head] = (unsigned char)(sample & 0xffu);
	audio_ring[(audio_ring_head + 1u) % AUDIO_RING_SIZE] =
	    (unsigned char)((sample >> 8) & 0xffu);
	audio_ring_head = (audio_ring_head + 2u) % AUDIO_RING_SIZE;
	audio_ring_count += 2u;
}

/*
 * audio_poll: drain I2S RX FIFO into ring buffer (PIO).
 * Returns number of samples pushed.
 */
static unsigned int
audio_poll(void)
{
	unsigned int level, n = 0u;
	unsigned int rdr;

	if (!audio_running)
		return 0u;
	level = i2s_read(I2S_RXFIFOLR) & RXFIFOLR_RFL_MASK;
	while (level-- > 0u) {
		/* RXDR: stereo frames (L+R); take L (mono). */
		rdr = i2s_read(I2S_RXDR);
		audio_ring_push((unsigned short)(rdr & 0xffffu));
		/* Skip R channel. */
		rdr = i2s_read(I2S_RXDR);
		(void)rdr;
		n++;
	}
	return n;
}

/*
 * i2s_rx_init: configure I2S RX for 16 kHz, 16-bit, stereo.
 */
static void
i2s_rx_init(void)
{
	unsigned int rxcr;

	/* Clear FIFOs. */
	i2s_write(I2S_CLR, CLR_RXC);

	/* RXCR: 16-bit, I2S format, 2ch, right-justified. */
	rxcr = RXCR_VDW(16) | RXCR_TFS_I2S | RXCR_CSR(1) | RXCR_SJM_R;
	i2s_write(I2S_RXCR, rxcr);

	/* CKR: RX-only master (divider TODO H4). */
	i2s_write(I2S_CKR, CKR_TRCM_RXONLY | CKR_MSS_MASTER);

	/* DMACR: DMA disabled (PIO mode for D2). */
	i2s_write(I2S_DMACR, 0u);
}

/*
 * audio_start: validate params, init I2S RX, enable XFER.
 * Only 16000 Hz, 16-bit, mono supported (F3 decision).
 */
int
audio_start(unsigned int rate, unsigned int bits, unsigned int ch)
{
	if (rate != 16000u || bits != 16u || ch != 1u)
		return -1;
	if (audio_running)
		return 0;
	i2s_rx_init();
	audio_ring_head = 0u;
	audio_ring_tail = 0u;
	audio_ring_count = 0u;
	audio_overrun = 0u;
	audio_running = 1;
	/* Enable RX transfer. */
	i2s_write(I2S_XFER, XFER_RX_EN);
	return 0;
}

void
audio_stop(void)
{
	i2s_write(I2S_XFER, XFER_RX_DIS);
	audio_running = 0;
}

/*
 * audio_read: copy up to len bytes from ring buffer to dst.
 * Polls the FIFO first. Returns bytes copied (0 = no data yet).
 */
int
audio_read(unsigned char *dst, unsigned int len)
{
	unsigned int n = 0u;
	unsigned int chunk;

	audio_poll();
	while (n < len && audio_ring_count > 0u) {
		chunk = audio_ring_count;
		if (chunk > len - n)
			chunk = len - n;
		/* Handle wrap. */
		{
			unsigned int first =
			    AUDIO_RING_SIZE - audio_ring_tail;
			unsigned int i;

			if (first > chunk)
				first = chunk;
			for (i = 0u; i < first; i++)
				dst[n + i] =
				    audio_ring[(audio_ring_tail + i) %
				    AUDIO_RING_SIZE];
			for (i = first; i < chunk; i++)
				dst[n + i] = audio_ring[i - first];
			audio_ring_tail =
			    (audio_ring_tail + chunk) % AUDIO_RING_SIZE;
			audio_ring_count -= chunk;
			n += chunk;
		}
	}
	return (int)n;
}

unsigned int
audio_overrun_count(void)
{
	return audio_overrun;
}

/*
 * D3: mic gain control.
 * level: 0 = 0dB, 1 = 12dB, 2 = 20dB (HW codes 0x1, 0x3, 0x2).
 * Applies to L+R. Returns 0/-1.
 */
int
audio_set_mic_gain(unsigned int level)
{
	unsigned int code;

	switch (level) {
	case 0u:
		code = 0x1u;	/* 0dB */
		break;
	case 1u:
		code = 0x3u;	/* 12dB */
		break;
	case 2u:
		code = 0x2u;	/* 20dB */
		break;
	default:
		return -1;
	}
	acodec_update_bits(ACODEC_ADC_ANA_CTL2,
	    ADC_L_MIC_GAIN_MSK | ADC_R_MIC_GAIN_MSK,
	    (code << ADC_L_MIC_GAIN_SFT) | (code << ADC_R_MIC_GAIN_SFT));
	return 0;
}
