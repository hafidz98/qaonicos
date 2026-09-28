/*
 * board.h - Board abstraction for the Mach ARMv7 port.
 *
 * Select exactly one board at compile time:
 *   -DBOARD_VIRT    : QEMU virt machine (default if neither is given)
 *   -DBOARD_RV1103  : Rockchip RV1103 / Luckfox Pico Mini
 *
 * Both targets are Cortex-A7 (ARMv7-A, 32-bit). All files are built
 * -ffreestanding without libc; this header contains only constants.
 */
#ifndef MACH_BOARD_H
#define MACH_BOARD_H

#if defined(BOARD_VIRT) && defined(BOARD_RV1103)
#error "board.h: select exactly one of BOARD_VIRT / BOARD_RV1103"
#endif

#if !defined(BOARD_VIRT) && !defined(BOARD_RV1103)
#define BOARD_VIRT 1
#endif

/* ------------------------------------------------------------------ */
/* QEMU virt (Cortex-A7)                                              */
/* ------------------------------------------------------------------ */
#if defined(BOARD_VIRT)

#define BOARD_NAME "qemu-virt"

/* PL011 UART0. */
#define UART0_BASE      0x09000000u
#define UART_BASE       UART0_BASE

/* GIC-400 (GICv2). */
#define GICD_BASE       0x08000000u
#define GICC_BASE       0x08010000u

/* RAM: 64 MiB mapped at 0x40000000 (emulasi Luckfox Pico Mini, QEMU -m 64).
 * Fase 12d: dulu 512MB, diturunkan agar peta memori guest persis 64MB. */
#define DRAM_BASE       0x40000000u
#define DRAM_SIZE       (64u * 1024u * 1024u)

#define BOARD_UART_IS_PL011 1

/* Fase 14: GPIO virtual (mock) untuk QEMU. Range MMIO palsu yang tidak
 * terpakai di peta memori QEMU -M virt (jendela virtio-mmio berakhir
 * di 0x0a004000, platform bus mulai 0x0c000000). Driver TIDAK menyentuh
 * alamat fisik ini di QEMU (unmapped -> data abort); register file
 * di-RAM-kan dengan offset yang sama persis seperti Rockchip (lihat
 * gpio.h). Base ini dipakai sebagai dokumentasi/penanda alamat. */
#define GPIO_VIRT_BASE  0x0a100000u
#define GPIO_VIRT_SIZE  0x1000u
#define GPIO_BANK_COUNT 1u

#endif /* BOARD_VIRT */

/* ------------------------------------------------------------------ */
/* Rockchip RV1103 / Luckfox Pico Mini                                */
/* ------------------------------------------------------------------ */
#if defined(BOARD_RV1103)

#define BOARD_NAME "rv1103"

/* DesignWare APB UART2 (8250/16550-compatible register layout). */
#define UART_BASE       0xff4c0000u
#define UART0_BASE      UART_BASE

/* GIC-400 (GICv2). */
#define GICD_BASE       0xff1f1000u
#define GICC_BASE       0xff1f2000u

/* RAM: 64 MiB mapped at 0x00000000. */
#define DRAM_BASE       0x00000000u
#define DRAM_SIZE       (64u * 1024u * 1024u)

#define BOARD_UART_IS_PL011 0

/* Fase 14: GPIO Rockchip (compatible "rockchip,gpio-bank", 32 pin/bank).
 * Alamat dari rv1106.dtsi (diwarisi rv1103.dtsi): gpio@ff380000 dst. */
#define GPIO0_BASE 0xff380000u
#define GPIO1_BASE 0xff530000u
#define GPIO2_BASE 0xff540000u
#define GPIO3_BASE 0xff550000u
#define GPIO4_BASE 0xff560000u
#define GPIO_BANK_COUNT 5u

#endif /* BOARD_RV1103 */

/* End of DRAM (first byte past the region). */
#define DRAM_END        (DRAM_BASE + DRAM_SIZE)

#endif /* MACH_BOARD_H */