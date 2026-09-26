# Alamat Hardware RV1103 (Luckfox Pico Mini)

Sumber: device tree resmi LuckfoxTECH/luckfox-pico, branch `main`.
Diekstrak via `grep` (file tidak dibaca utuh). Salinan file ada di `dts/`.

- RV1103 (`rv1103.dtsi`) pada dasarnya hanya mewarisi (`#include`) `rv1106.dtsi`
  dan meng-override sebagian. Jadi node inti SoC (gic, cru, grf, pmu, uart)
  didefinisikan di `rv1106.dtsi`.
- Board target `rv1103g-luckfox-pico-mini.dts` meng-include:
  `rv1103.dtsi` -> `rv1106-evb.dtsi` -> `rv1106-amp.dtsi`, dan
  `rv1103-luckfox-pico-ipc.dtsi` -> `rv1106-amp.dtsi`.

## Tabel alamat

| Peripheral | compatible | base | size | file sumber |
|---|---|---|---|---|
| GIC (GICD) | `arm,gic-400` (`interrupt-controller@ff1f0000`) | `0xff1f1000` | `0x1000` | `rv1106.dtsi` (baris 489-499) |
| GIC CPU I/F (GICC) | `arm,gic-400` | `0xff1f2000` | `0x2000` | `rv1106.dtsi` |
| GIC (VIF/others slot 3) | `arm,gic-400` | `0xff1f4000` | `0x2000` | `rv1106.dtsi` |
| GIC (slot 4) | `arm,gic-400` | `0xff1f6000` | `0x2000` | `rv1106.dtsi` |
| Timer (CPU armv7) | `arm,armv7-timer` | tidak ada `reg` (diakses via PPI, bukan MMIO) | - | `rv1106.dtsi` (baris 419-424) |
| UART2 | `rockchip,rv1106-uart`, `snps,dw-apb-uart` (`serial@ff4c0000`) | `0xff4c0000` | `0x100` | `rv1106.dtsi` (baris 999-1011) |
| Memory (DRAM) | `memory` (`device_type = "memory"`) | `0x00000000` | `0x08000000` (128 MB) | `rv1106-thunder-boot.dtsi` (baris 7-10), juga `rv1106-tb-nofastae.dtsi` |
| Memory (override board) | `memory` | `0x00000000` | `0x04000000` (64 MB) | `rv1103g-evb2-v10.dts` (baris 159-160) |
| CRU | `rockchip,rv1106-cru` (`clock-controller@ff3a0000`) | `0xff3a0000` | `0x20000` | `rv1106.dtsi` (baris 697-702) |
| GRF | `rockchip,rv1106-grf`, `syscon`, `simple-mfd` (`syscon@ff000000`) | `0xff000000` | `0x68000` | `rv1106.dtsi` (baris 426-430) |
| PMU | `rockchip,rv1106-pmu`, `syscon` (`power-management@ff300000`) | `0xff300000` | `0x1000` | `rv1106.dtsi` (baris 522-525) |

## Yang TIDAK ketemu

- **Timer MMIO terpisah** (mis. `dw-apb-timer` / `timer@xxxx`): TIDAK ADA di
  device tree. RV1103 hanya memakai `arm,armv7-timer` (CPU generic timer,
  diakses melalui interrupt PPI 13/14, tanpa `reg` MMIO).
- **Node `memory` di `rv1106.dtsi`/`rv1103.dtsi`/`rv1103g-luckfox-pico-mini.dts`**:
  TIDAK ADA. Node `memory` hanya muncul di varian thunder-boot
  (`rv1106-thunder-boot.dtsi`, `rv1106-tb-nofastae.dtsi`) dan di-override di
  board (`rv1103g-evb2-v10.dts`). Untuk Luckfox Pico Mini, alamat/size DRAM
  biasanya di-set oleh U-Boot / bootargs, bukan di DTS board.
- **U-Boot DTS `rv1106.dtsi`** juga tidak mendefinisikan node `memory`
  (hanya `reserved-memory`).
- **PMU/GRF "sub-node" terpisah** (mis. `pmugrf`) tidak ada; GRF tunggal
  mencakup `0xff000000` selama `0x68000`, dan PMU terpisah di `0xff300000`.

## Catatan konfirmasi

- **UART2 = `0xff4c0000` — TERKONFIRMASI** dari `reg = <0xff4c0000 0x100>`
  pada node `uart2: serial@ff4c0000` di `rv1106.dtsi`.
- Bootargs di `rv1103-luckfox-pico-ipc.dtsi` juga memakai
  `earlycon=uart8250,mmio32,0xff4c0000 console=ttyFIQ0`, konsisten.

## File sumber (salinan lokal di `dts/`)

- `rv1103.dtsi`, `rv1106.dtsi`, `rv1106-amp.dtsi`, `rv1106-evb.dtsi`,
  `rv1106-ipc.dtsi`, `rv1103-luckfox-pico-ipc.dtsi`,
  `rv1103g-luckfox-pico-mini.dts`, `rv1103g-luckfox-pico.dts`,
  `rv1103g-38x38-ipc-v10.dts`, `rv1106g-38x38-ipc-v10.dts`,
  `rv1106g3.dtsi`, `rv1106-evb-v10.dtsi`, `rv1103g-evb2-v10.dts`,
  `rv1106-thunder-boot.dtsi`, `rv1106-tb-nofastae.dtsi`
- U-Boot (lokal, prefix `uboot-`): `uboot-rv1106.dtsi`, `uboot-rv1106-u-boot.dtsi`,
  `uboot-rv1106-luckfox.dts`, `uboot-rv1106-evb.dts`

URL basis:
- Kernel DTS: `https://raw.githubusercontent.com/LuckfoxTECH/luckfox-pico/main/sysdrv/source/kernel/arch/arm/boot/dts/`
- U-Boot DTS: `https://raw.githubusercontent.com/LuckfoxTECH/luckfox-pico/main/sysdrv/source/uboot/u-boot/arch/arm/dts/`