/*
 * user/upl330.c -- D2b: PL330 microcode structure test.
 *
 * One-shot: pl330_init -> pl330_audio_rx -> verify microcode bytes
 * (opcodes in order, SAR=I2S_RXDR, DAR=dst, CCR sane).
 * Prints UPL330: PASS/FAIL.
 *
 * NOTE: DMAGO execution not yet implemented (TODO H4). This test
 * verifies microcode ASSEMBLY, not DMA execution.
 */
#include "ulib/ulib.h"

/* Syscalls 91/92 for ucode inspection (test only). */
static unsigned
sys_uclen(void)
{
	unsigned ret;
	__asm__ volatile(
		"mov r7, #91\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : : "r7", "lr", "memory", "cc");
	return ret;
}

static unsigned
sys_ucbyte(unsigned i)
{
	register unsigned _r0 __asm__("r0") = i;
	unsigned ret;
	__asm__ volatile(
		"mov r7, #92\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0) : "r7", "lr", "memory", "cc");
	return ret;
}

static unsigned char dmabuf[1024] __attribute__((aligned(4)));

static void
puthex(unsigned v)
{
	static const char *h = "0123456789abcdef";
	char b[11];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 8; i++)
		b[2 + i] = h[(v >> (28 - i * 4)) & 0xFu];
	b[10] = '\n';
	sys_write(1, b, 11u);
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int r, fails = 0;
	unsigned len, i;
	unsigned char uc[64];

	u_put("upl330: PL330 microcode test (D2b)\n");

	r = sys_pl330_init();
	u_put("  pl330_init -> ");
	puthex((unsigned)r);
	if (r != 0)
		fails++;

	r = sys_pl330_rx(dmabuf, 16u);
	u_put("  pl330_rx -> ");
	puthex((unsigned)r);
	if (r != 0)
		fails++;

	len = sys_uclen();
	u_put("  ucode len=");
	puthex(len);
	if (len == 0u || len > 64u)
		fails++;

	/* Verifikasi struktur via kernel (a0=0xFFFFFFFF). */
	r = (int)sys_ucbyte(0xffffffffu);
	u_put("  verify mask=");
	puthex((unsigned)r);
	if (r != 0)
		fails++;
	/* Invalid args must fail */
	if (sys_pl330_rx(dmabuf, 0u) == 0) {
		u_put("  BAD: nwords=0 accepted\n");
		fails++;
	}
	if (sys_pl330_rx(dmabuf, 300u) == 0) {
		u_put("  BAD: nwords>256 accepted\n");
		fails++;
	}

	if (fails == 0)
		u_put("UPL330: PASS\n");
	else
		u_put("UPL330: FAIL\n");
	sys_exit(fails);
}
