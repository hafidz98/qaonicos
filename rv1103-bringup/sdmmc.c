/*
 * sdmmc.c - Driver SDMMC asli RV1103 (DesignWare Mobile Storage Host,
 * "rockchip,rv1106-dw-mshc"), Fase 15.
 *
 * Base: 0xffaa0000 (size 0x4000), dari rv1106.dtsi
 * ("sdmmc: mmc@ffaa0000", compatible rockchip,rv1106-dw-mshc) —
 * RV1103 mewarisi rv1106.dtsi (lihat hw-addrs.md). IRQ GIC_SPI 52.
 *
 * STATUS: compile-check saja (clang --target=arm-none-eabi). TIDAK
 * di-run di QEMU (QEMU -M virt tidak punya DW-MSHC; backend QEMU =
 * virtio-blk kedua di blk.c). Belum dites di hardware nyata:
 * urutan init clock (cru HCLK_SDIO/CCLK_SRC_SDIO) dan pinctrl
 * (sdmmc0_clk/cmd/det/bus4) masih asumsi dari DTS.
 *
 * Transfer polling single-block (CMD17 baca / CMD24 tulis, 512B);
 * tanpa DMA (PIO via FIFO) agar tidak perlu descriptor IDMAC.
 */
#include <stdint.h>

#include "sdmmc.h"

#define SDMMC_BASE 0xffaa0000u

/* Register DW-MSHC (offset dari base). */
#define SD_CTRL    0x000u
#define SD_PWREN   0x004u
#define SD_CLKDIV  0x008u
#define SD_CLKENA  0x010u
#define SD_TMOUT   0x014u
#define SD_CTYPE   0x018u
#define SD_BLKSIZ  0x01cu
#define SD_BYTCNT  0x020u
#define SD_INTMSK  0x024u
#define SD_CMDARG  0x028u
#define SD_CMD     0x02cu
#define SD_RESP0   0x030u
#define SD_RINTSTS 0x044u
#define SD_STATUS  0x048u
#define SD_FIFOTH  0x04cu
#define SD_DATA    0x200u   /* FIFO data (32-bit port) */

/* Bit CTRL. */
#define CTRL_RESET_ALL   (1u << 0)
#define CTRL_FIFO_RESET  (1u << 1)
#define CTRL_DMA_RESET   (1u << 2)
/* Bit CMD. */
#define CMD_START        (1u << 31)
#define CMD_RESP_EXP     (1u << 6)
#define CMD_RESP_LONG    (1u << 7)
#define CMD_DATA_EXP     (1u << 9)
#define CMD_RW           (1u << 10)   /* 1 = baca (card->host) */
#define CMD_SEND_INIT    (1u << 15)
/* Bit RINTSTS. */
#define RINT_CMD_DONE    (1u << 2)
#define RINT_DTO         (1u << 3)    /* data transfer over */
#define RINT_RTO         (1u << 8)    /* response timeout */
#define RINT_DCRC        (1u << 7)
#define RINT_RCRC        (1u << 6)
/* Bit STATUS. */
#define STATUS_FIFO_FULL (1u << 3)
#define STATUS_FIFO_EMPTY (1u << 2)

static volatile uint32_t *const sdreg = (volatile uint32_t *)SDMMC_BASE;

static inline uint32_t sdr(uint32_t off)
{
    return sdreg[off / 4u];
}
static inline void sdw(uint32_t off, uint32_t v)
{
    sdreg[off / 4u] = v;
}
static void sbarrier(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
}

/* Tunggu sampai (RINTSTS & mask) != 0 atau timeout habis.
 * Kembalikan 0 bila mask terpenuhi. */
static int sd_wait_rint(uint32_t mask, unsigned spins)
{
    while (spins-- > 0u) {
        if (sdr(SD_RINTSTS) & mask)
            return 0;
    }
    return -1;
}

/* Kirim satu perintah MMC/SD. cmd = nomor perintah; arg = argumen;
 * flags = kombinasi CMD_* di atas (tanpa CMD_START).
 * Kembalikan 0 bila CMD_DONE tanpa error response. */
static int sd_send_cmd(unsigned cmd, uint32_t arg, uint32_t flags)
{
    unsigned spins;

    sdw(SD_CMDARG, arg);
    sdw(SD_CMD, (cmd & 0x3fu) | flags | CMD_START);
    sbarrier();

    spins = 1000000u;
    while (spins-- > 0u) {
        if (sdr(SD_CMD) & CMD_START)
            continue;
        break;
    }
    if (spins == 0u)
        return -1;                      /* controller sibuk */
    if (sd_wait_rint(RINT_CMD_DONE, 1000000u) != 0)
        return -2;                      /* command timeout */
    if (sdr(SD_RINTSTS) & (RINT_RTO | RINT_RCRC))
        return -3;                      /* response error */
    return 0;
}

int sdmmc_init(void)
{
    unsigned spins;

    /* Reset controller + FIFO + DMA. */
    sdw(SD_CTRL, CTRL_RESET_ALL | CTRL_FIFO_RESET | CTRL_DMA_RESET);
    spins = 100000u;
    while ((sdr(SD_CTRL) &
            (CTRL_RESET_ALL | CTRL_FIFO_RESET | CTRL_DMA_RESET)) &&
           spins-- > 0u)
        ;
    if (spins == 0u)
        return -1;

    /* Asumsi: clock SDMMC sudah di-enable via CRU oleh bootloader
     * (HCLK_SDIO/CCLK_SRC_SDIO, lihat rv1106.dtsi baris 1188).
     * Set divider aman untuk fase identifikasi (<= 400 kHz). */
    sdw(SD_CLKENA, 0u);                 /* matikan clock dulu */
    sdw(SD_CLKDIV, 0x7du);              /* divider besar = clock lambat */
    sdw(SD_CLKENA, 0x1u);               /* enable cclk_out */
    sdw(SD_PWREN, 0x1u);                /* power on */
    sdw(SD_TMOUT, 0xffffffffu);
    sdw(SD_CTYPE, 0x1u);                /* bus 4-bit (pinctrl sdmmc0_bus4) */
    sdw(SD_BLKSIZ, 512u);
    sdw(SD_INTMSK, 0u);                 /* polling, tanpa IRQ */
    sbarrier();

    /* CMD0: reset card ke idle. CMD8/ACMD41 dst = init card penuh
     * (belum diimplementasi — butuh di hardware nyata). */
    if (sd_send_cmd(0u, 0u, CMD_SEND_INIT) != 0)
        return -2;
    return 0;
}

/* Transfer satu blok 512B. is_read=1 -> CMD17, =0 -> CMD24.
 * addr = nomor blok (SDHC/SDXC: block addressing). */
static int sd_xfer_block(uint32_t blk, uint8_t *buf, int is_read)
{
    unsigned i, spins;
    uint32_t w;

    sdw(SD_BLKSIZ, 512u);
    sdw(SD_BYTCNT, 512u);
    /* Bersihkan status interrupt sebelum transfer. */
    sdw(SD_RINTSTS, 0xffffffffu);
    sbarrier();

    if (sd_send_cmd(is_read ? 17u : 24u, blk,
                    CMD_RESP_EXP | CMD_DATA_EXP |
                    (is_read ? CMD_RW : 0u)) != 0)
        return -1;

    if (is_read) {
        for (i = 0; i < 128u; i++) {
            spins = 100000u;
            while ((sdr(SD_STATUS) & STATUS_FIFO_EMPTY) && spins-- > 0u)
                ;
            if (spins == 0u)
                return -2;
            w = sdr(SD_DATA);
            buf[i * 4u + 0u] = (uint8_t)w;
            buf[i * 4u + 1u] = (uint8_t)(w >> 8);
            buf[i * 4u + 2u] = (uint8_t)(w >> 16);
            buf[i * 4u + 3u] = (uint8_t)(w >> 24);
        }
    } else {
        for (i = 0; i < 128u; i++) {
            spins = 100000u;
            while ((sdr(SD_STATUS) & STATUS_FIFO_FULL) && spins-- > 0u)
                ;
            if (spins == 0u)
                return -2;
            w = (uint32_t)buf[i * 4u + 0u] |
                ((uint32_t)buf[i * 4u + 1u] << 8) |
                ((uint32_t)buf[i * 4u + 2u] << 16) |
                ((uint32_t)buf[i * 4u + 3u] << 24);
            sdw(SD_DATA, w);
        }
    }
    sbarrier();
    if (sd_wait_rint(RINT_DTO, 1000000u) != 0)
        return -3;
    if (sdr(SD_RINTSTS) & RINT_DCRC)
        return -4;
    return 0;
}

int sdmmc_read_sector(uint32_t sector, uint8_t *buf)
{
    if (!buf)
        return -1;
    return sd_xfer_block(sector, buf, 1);
}

int sdmmc_write_sector(uint32_t sector, const uint8_t *buf)
{
    uint8_t tmp[512];
    unsigned i;
    if (!buf)
        return -1;
    /* xfer memakai buffer u8 mutable; salin dulu (tanpa string.h). */
    for (i = 0; i < 512u; i++)
        tmp[i] = buf[i];
    return sd_xfer_block(sector, tmp, 0);
}
