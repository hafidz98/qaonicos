/*
 * mach3-port/src/ext/pl330.c -- Driver minimal ARM PL330 DMAC (D2b).
 *
 * Untuk audio: I2S RX (peripheral 21) -> RAM. Pola: microcode di RAM,
 * channel di-start via DMAGO (manager thread).
 *
 * Referensi: ARM DDI0424 (PL330 TRM), rockchip-linux pl330.c.
 * Hanya yang dipakai audio; bukan driver DMA generik.
 */
#include "pl330.h"

/* --- Register DMAC (offset dari base) --- */
#define	PL330_DS	0x000u	/* DMA Status (RO) */
#define	PL330_DPC	0x004u	/* DMA Program Counter */
#define	PL330_INTEN	0x008u
#define	PL330_ES	0x024u	/* Event Status */
#define	PL330_CSR	0x02cu	/* Channel Status */
#define	PL330_CPC	0x030u	/* Channel PC */
#define	PL330_SAR	0x034u
#define	PL330_DAR	0x038u
#define	PL330_CCR	0x03cu
#define	PL330_LC0	0x040u
#define	PL330_LC1	0x044u

/* Debug interface. */
#define	PL330_DBGSTATUS	0x0d00u
#define	PL330_DBGCMD	0x0d04u
#define	PL330_DBGINST0	0x0d08u
#define	PL330_DBGINST1	0x0d0cu

/* Config. */
#define	PL330_CR0	0x0e00u
#define	PL330_CR1	0x0e04u
#define	PL330_CRD	0x0e08u

/* --- Opcode microcode --- */
#define	OP_DMAADDH	0x54u
#define	OP_DMAEND	0x00u
#define	OP_DMAFLUSHP	0x35u
#define	OP_DMAGO	0xa0u
#define	OP_DMALD	0x04u
#define	OP_DMALDP	0x25u
#define	OP_DMALP	0x20u
#define	OP_DMALPEND	0x28u
#define	OP_DMANOP	0x18u
#define	OP_DMAST	0x08u
#define	OP_DMASTP	0x29u
#define	OP_DMAWFP	0x30u
#define	OP_DMAMOV	0xbcu

/* DMAMOV destination. */
#define	MOV_SAR		0x00u
#define	MOV_CCR		0x01u
#define	MOV_DAR		0x02u

/* DMAWFP/DMAFLUSHP modifier. */
#define	BURST		1u
#define	SINGLE		0u

/* CCR bits. */
#define	CCR_SRC_INC	(1u << 0)
#define	CCR_DST_INC	(1u << 1)
#define	CCR_SRC_W(w)	((w) << 2)	/* 0=8, 1=16, 2=32 bit */
#define	CCR_DST_W(w)	((w) << 4)
#define	CCR_SRC_B(b)	((b) << 6)	/* burst len = 2^b */
#define	CCR_DST_B(b)	((b) << 10)

#if defined(BOARD_RV1103)
#define	DMAC_BASE	0xff420000u
static volatile unsigned int *const dmac =
    (volatile unsigned int *)DMAC_BASE;
#else
/* QEMU: virtual register file. */
static volatile unsigned int dmac_virt[0x1000u];
static volatile unsigned int *const dmac = dmac_virt;
#endif

#define	DMAC_REG(o)	(dmac[(o) / 4u])

/* Microcode buffer (di RAM, accessible DMAC). */
#define	UCODE_SIZE	64u
unsigned char pl330_ucode_buf[UCODE_SIZE] __attribute__((aligned(8)));
unsigned int pl330_ucode_buflen;
#define	ucode	pl330_ucode_buf
#define	ucode_len	pl330_ucode_buflen

/* Peripheral request number untuk I2S RX (dari DTS). */
#define	PERI_I2S_RX	21u

/*
 * pl330_init: baca konfigurasi, pastikan DMAC idle.
 * Return 0 ok, -1 bila DMAC sibuk/fault.
 */
int
pl330_init(void)
{
	unsigned int ds;

	ds = DMAC_REG(PL330_DS);
	/* DS[3:0] = state manager; 0 = stopped/idle. */
	if ((ds & 0xfu) != 0u)
		return -1;
	return 0;
}

/*
 * pl330_audio_rx: program DMA I2S RX -> RAM.
 *   dst: alamat RAM (harus 4-byte aligned).
 *   nwords: jumlah word 32-bit (I2S_RXDR dibaca sebagai 32-bit,
 *           software ekstrak 16-bit L).
 * Return 0 bila channel di-start, -1 bila gagal.
 *
 * Microcode:
 *   DMAMOV SAR, I2S_RXDR
 *   DMAMOV DAR, dst
 *   DMAMOV CCR, (src fixed, dst inc, 32-bit, burst 1)
 *   DMALP nwords
 *     DMAWFP SINGLE, PERI_I2S_RX
 *     DMALDP SINGLE, PERI_I2S_RX
 *     DMAST
 *   DMALPEND
 *   DMAEND
 */
int
pl330_audio_rx(unsigned int dst, unsigned int nwords)
{
	unsigned char *p = ucode;
	unsigned int ccr;
	unsigned int it;

	if (dst & 3u || nwords == 0u || nwords > 256u)
		return -1;

	/* DMAMOV SAR, 0xFFAE0028 */
	*p++ = OP_DMAMOV;
	*p++ = MOV_SAR;
	*p++ = 0x28u; *p++ = 0x00u; *p++ = 0xaeu; *p++ = 0xffu;

	/* DMAMOV DAR, dst */
	*p++ = OP_DMAMOV;
	*p++ = MOV_DAR;
	*p++ = (unsigned char)(dst & 0xffu);
	*p++ = (unsigned char)((dst >> 8) & 0xffu);
	*p++ = (unsigned char)((dst >> 16) & 0xffu);
	*p++ = (unsigned char)((dst >> 24) & 0xffu);

	/* DMAMOV CCR */
	ccr = CCR_DST_INC | CCR_SRC_W(2) | CCR_DST_W(2) |
	    CCR_SRC_B(0) | CCR_DST_B(0);
	/* src_inc=0 (peripheral fixed), dst_inc=1. */
	*p++ = OP_DMAMOV;
	*p++ = MOV_CCR;
	*p++ = (unsigned char)(ccr & 0xffu);
	*p++ = (unsigned char)((ccr >> 8) & 0xffu);
	*p++ = (unsigned char)((ccr >> 16) & 0xffu);
	*p++ = (unsigned char)((ccr >> 24) & 0xffu);

	/* DMALP: loop nwords kali (max 256 per LP0). */
	it = nwords > 256u ? 256u : nwords;
	*p++ = OP_DMALP | 0x00u;	/* LP0 */
	*p++ = (unsigned char)(it - 1u);

	/* DMAWFP SINGLE, peri */
	*p++ = OP_DMAWFP | (SINGLE << 1);
	*p++ = (unsigned char)PERI_I2S_RX;

	/* DMALDP SINGLE, peri */
	*p++ = OP_DMALDP | (SINGLE << 1);
	*p++ = (unsigned char)PERI_I2S_RX;

	/* DMAST (MFIFO -> DAR, inc) */
	*p++ = OP_DMAST;

	/* DMALPEND */
	*p++ = OP_DMALPEND | 0x00u;	/* LP0 */
	*p++ = 0x00u;	/* backward jump placeholder */

	/* DMAEND */
	*p++ = OP_DMAEND;

	ucode_len = (unsigned int)(p - ucode);

	/* TODO: issue DMAGO via manager/debug. Untuk D2b tahap 1,
	 * microcode dirakit dan diverifikasi strukturnya di QEMU.
	 * Eksekusi aktual butuh DMAGO yang benar — validasi di H4. */
	return 0;
}

/*
 * pl330_ucode_len: panjang microcode yang dirakit (untuk test).
 */
unsigned int
pl330_ucode_len(void)
{
	return ucode_len;
}

/*
 * pl330_ucode_byte: baca byte microcode (untuk test verifikasi).
 */
unsigned int
pl330_ucode_byte(unsigned int idx)
{
	if (idx >= UCODE_SIZE)
		return 0u;
	return ucode[idx];
}
