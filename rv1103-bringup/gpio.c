/*
 * gpio.c - Driver GPIO QaonicOS, dua backend (Fase 14).
 *
 * Offset register Rockchip (sama untuk kedua backend):
 *   SW_PORTx_DR  = base + port*8        (data)
 *   SW_PORTx_DDR = base + port*8 + 4     (direction: 0=input, 1=output)
 *   EXT_PORTx    = base + 0x50 + port*4  (baca level pin)
 * dengan port = pin/8 (0..3 -> A..D), bit = pin%8.
 *
 * Backend BOARD_RV1103: base = alamat MMIO asli per bank (board.h).
 * Backend BOARD_VIRT:   base = register file di RAM (lihat gpio.h
 * kenapa MMIO fisik tidak disentuh di QEMU).
 */
#include "gpio.h"

#include <stdint.h>

/* --- offset register (Rockchip) ------------------------------------ */
#define RK_DR_OFF(port)  ((uint32_t)(port) * 8u)
#define RK_DDR_OFF(port) ((uint32_t)(port) * 8u + 4u)
#define RK_EXT_OFF(port) (0x50u + (uint32_t)(port) * 4u)
#define RK_BANK_SIZE     0x60u   /* cukup untuk EXT_PORTD */

#if defined(BOARD_RV1103)
/* Base MMIO asli per bank (board.h). */
static volatile uint32_t *const gpio_bases[GPIO_BANK_COUNT] = {
    (volatile uint32_t *)GPIO0_BASE,
    (volatile uint32_t *)GPIO1_BASE,
    (volatile uint32_t *)GPIO2_BASE,
    (volatile uint32_t *)GPIO3_BASE,
    (volatile uint32_t *)GPIO4_BASE,
};
#else
/* BOARD_VIRT: register file di RAM, offset identik Rockchip. */
static volatile uint32_t gpio_virt_regs[GPIO_BANK_COUNT][RK_BANK_SIZE / 4u];
#endif

static volatile uint32_t *gpio_base(unsigned bank)
{
#if defined(BOARD_RV1103)
    return gpio_bases[bank];
#else
    return &gpio_virt_regs[bank][0];
#endif
}

void gpio_init(void)
{
#if !defined(BOARD_RV1103)
    unsigned b, i;
    /* BSS sudah nol; loop eksplisit agar init terdokumentasi dan
     * deterministik bila dipanggil ulang. */
    for (b = 0u; b < GPIO_BANK_COUNT; b++)
        for (i = 0u; i < RK_BANK_SIZE / 4u; i++)
            gpio_virt_regs[b][i] = 0u;
#else
    /* Hardware asli: tidak disentuh (clock gate = urusan bootloader). */
#endif
}

int gpio_set(unsigned bank, unsigned pin, unsigned val)
{
    volatile uint32_t *base;
    volatile uint8_t *reg8;
    unsigned port, bit;

    if (bank >= GPIO_BANK_COUNT || pin >= GPIO_PINS_PER_BANK)
        return -1;
    port = pin / 8u;
    bit = pin % 8u;
    base = gpio_base(bank);

    /* 1. Jadikan output: set bit di DDR. */
    reg8 = (volatile uint8_t *)base + RK_DDR_OFF(port);
    *reg8 = (uint8_t)(*reg8 | (uint8_t)(1u << bit));

    /* 2. Drive level: tulis bit di DR. */
    reg8 = (volatile uint8_t *)base + RK_DR_OFF(port);
    if (val != 0u)
        *reg8 = (uint8_t)(*reg8 | (uint8_t)(1u << bit));
    else
        *reg8 = (uint8_t)(*reg8 & (uint8_t)~(1u << bit));

#if !defined(BOARD_RV1103)
    /* Mock: EXT_PORT mencerminkan level yang di-drive (di hardware
     * asli, membaca EXT_PORT pin output mengembalikan level output). */
    reg8 = (volatile uint8_t *)base + RK_EXT_OFF(port);
    if (val != 0u)
        *reg8 = (uint8_t)(*reg8 | (uint8_t)(1u << bit));
    else
        *reg8 = (uint8_t)(*reg8 & (uint8_t)~(1u << bit));
#endif
    return 0;
}

int gpio_get(unsigned bank, unsigned pin)
{
    volatile uint32_t *base;
    volatile uint8_t *reg8;
    unsigned port, bit;

    if (bank >= GPIO_BANK_COUNT || pin >= GPIO_PINS_PER_BANK)
        return -1;
    port = pin / 8u;
    bit = pin % 8u;
    base = gpio_base(bank);

    reg8 = (volatile uint8_t *)base + RK_EXT_OFF(port);
    return ((*reg8 >> bit) & 1u) ? 1 : 0;
}
