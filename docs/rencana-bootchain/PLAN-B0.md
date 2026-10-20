# B0 Plan: Boot Chain + SD Card Support

Date: 2026-10-20. Goal: QaonicOS boots on Luckfox Pico Mini B hardware
"out of the box" — no QEMU, real board, real boot.

Research: `docs/riset-bootchain-rv1103/RINGKASAN.md`,
`docs/riset-out-of-box/RINGKASAN.md`.

## 1. Boot chain (what happens before our code runs)

```
Power on
  -> MaskROM (silicon, always works, unbrickable)
    -> idbloader (sector 64 / 0x8000 on boot media)
      -> U-Boot
        -> QaonicOS kernel
```

### 1.1 Components we REUSE (do not build)
- **MaskROM**: in silicon. Entry point for recovery flashing.
- **idbloader**: Rockchip closed binary from `rockchip-linux/rkbin`,
  `bin/rv11/`. Contains DDR init (`rv1103b_ddr_924MHz_v1.07.bin`,
  chip-specific calibration — cannot be rebuilt) + SPL
  (`rv1103b_spl_v1.01.bin`). Assembled with `boot_merger` using
  `RKBOOT/RV1103BMINIALL.ini`.
- **U-Boot**: Luckfox SDK build (has DRAM, NAND, SD, USB drivers).

**Decision: keep Rockchip idbloader + Luckfox U-Boot as-is.
Only replace what runs after U-Boot.** This is the fastest path to
a booting board and keeps maskrom recovery intact.

### 1.2 What we provide
- `qaonic_kernel.bin` — our Mach kernel, loaded by U-Boot.
- `parameter.txt` — partition layout (from SDK, adjusted).
- `update.img` — packaged image for flashing (afptool).

## 2. Boot methods

### 2.1 IMPORTANT CORRECTION (2026-10-20)
The Mini B does **NOT** have a TF/microSD slot. Only the Mini A does.
- Mini A: TF card slot, no NAND.
- Mini B: 128MB SPI NAND, **no TF slot**.

The RV1103 chip has an SDMMC controller (`0xffaa0000`), but on the
Mini B board those pins are not routed to a card slot. SD boot is
therefore **not available on Mini B**. The plan below is adjusted:
NAND is the only boot path for Mini B.

### 2.2 NAND boot (Mini B — only path)
1. Hold BOOT button + plug USB -> MaskROM mode (VID 0x2207).
2. Flash `update.img`:
   - Linux: `upgrade_tool uf update.img` or `rkdeveloptool`.
   - Windows: SocToolKit (select RV1103).
3. Reboot — boots from NAND.

SPI NAND partition layout (from SDK):
```
256K(env), 256K@256K(idblock), 512K(uboot), 4M(boot),
30M(oem), 6M(userdata), 85M(rootfs)
```

### 2.3 SD boot (Mini A only — for reference)
If a Mini A is ever used: create bootable SD (SocToolKit "SD Card
Boot" mode), insert, power on. BootROM loads idbloader from SD,
U-Boot loads the kernel from the SD FAT partition.

## 3. Kernel loading via U-Boot

U-Boot interactive (for first bring-up):
```
fatload mmc 0:1 0x40000000 qaonic_kernel.bin
go 0x40000000
```

Automated: set `bootcmd` to the above + `saveenv`.

**Open questions (resolve during B0 implementation):**
- Q1: Is the `go` command enabled in Luckfox U-Boot? (If not, use
  `boot.img` Android format instead.)
- Q2: Kernel load address — is 0x40000000 correct for RV1103 DDR
  map, or does U-Boot expect a different address?
- Q3: Debug UART pins on Mini B (for serial console during bring-up).
- Q4: SD vs NAND boot priority — test empirically.

## 4. SD card support in QaonicOS

### 4.1 Hardware reality
- **Mini B (your board)**: No TF slot. SDMMC controller exists in the
  chip (`0xffaa0000`) but pins are not routed to a slot. **SD cards
  cannot be used on Mini B** without hardware modification.
- **Mini A**: Has TF slot. SD is the primary storage.

### 4.2 Current driver state
- QEMU: SD works via virtio-blk (`blk.c` dev 1), FAT32 on top
  (`fat32.c`). Test `ufs` PASS. Shell `ls`/`cat` work on `/sd`.
- Hardware (Mini A): **no SDMMC driver yet.** Needs a DesignWare MSHC
  driver for `0xffaa0000`. The existing `fat32.c` works unchanged once
  `sd_read`/`sd_write` are backed by real hardware.

### 4.3 How the user would use an SD card (Mini A, or future board)
- **Insert card** -> auto-detected, mounted at `/sd`.
- **Shell**: `ls /sd`, `cat /sd/file.txt` (already works in QEMU).
- **Data**: user files, logs, audio recordings on SD.
- **Format**: FAT32 (works in any PC — copy files directly).

### 4.4 For Mini B: what replaces SD?
Since Mini B has no slot, removable/user storage options are:
- **USB mass storage** (USB 2.0 port) — needs USB host + MSC driver.
- **NAND data partition** (`userdata`, 6M in the layout) — internal,
  not removable.
- **Network** (via C6 WiFi) — cloud sync.

**Recommendation**: For Mini B, treat USB storage as the "SD card
equivalent" for user files. SDMMC driver is only needed if Mini A
support is ever required.

## 5. B0 tasks

- [x] B0.1 Obtain SDK files — DONE 2026-10-20:
      rkbin cloned; idbloader built (320 KB). Luckfox SDK sparse
      checkout (841 MB): partition layout extracted from
      BoardConfig-SPI_NAND-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk.
      parameter.txt generated. **U-Boot BUILT 2026-10-20**: plain make
      with arm-none-eabi (defconfig luckfox_rv1106_uboot_defconfig,
      KCFLAGS -Wno-error -fcommon, bc from apt download) ->
      u-boot.bin 397 KB -> loaderimage pack at 0x00200000 (matches
      CONFIG_SYS_TEXT_BASE) -> `uboot.img` 2 MB.
      All artifacts in `~/workspace/b0-sdk/`: idbloader.img,
      uboot.img, qaonic_kernel.bin, parameter.txt.
- [x] B0.2 Build `qaonic_kernel.bin` — DONE 2026-10-20:
      `llvm-objcopy -O binary mach3.elf` -> 3.7 MB (fits 4M boot
      partition with ~450 KB spare). At `~/workspace/b0-sdk/`.
- [x] B0.3 Package `update.img` — DONE 2026-10-20: (see also the reproducible
      build script below)
      afptool pack (after fixing parameter.txt sector offsets) +
      rkImageMaker wrap -> `update-final.img` (7.5 MB) in
      `~/workspace/b0-sdk/`. Contents: download.bin (idbloader),
      parameter, uboot.img, boot.img (= qaonic_kernel.bin),
      rootfs.img (1 MB zero placeholder — QaonicOS embeds its
      files in the kernel image; proper rootfs partition TBD).
- [ ] B0.4 Flash via maskrom; verify U-Boot loads kernel.
- [ ] B0.5 Resolve open questions (U-Boot `go` availability, load
      address, debug UART pins).
- [ ] B0.6 Write flashing guide (docs) for NAND path.
- [ ] B0.7 Hardware test entries -> `docs/rencana-uji-hardware/`
      (boot from NAND, maskrom recovery).

## 6. Acceptance criteria
- Board boots from NAND to `qaon>` prompt after flashing `update.img`.
- Maskrom recovery still works after a bad flash (unbrickable).
- Kernel banner + logo show on serial console.

## 7. Out of scope for B0 (later phases)
- SDMMC driver in the OS (B1).
- littlefs on NAND (B1).
- Watchdog/boot counter (B2).
- Provisioning (B3).
- A/B updates (B4).

## Reproducible build (2026-10-20)
- `scripts/build-update-img.sh` runs the whole pipeline in one
  command: kernel build -> raw binary -> idbloader (boot_merger)
  -> package tree -> afptool -> rkImageMaker -> update-final.img.
  Canonical partition layout: `scripts/update-img/parameter.txt`.
  Verified end-to-end: update-final.img = 7,624,842 bytes with
  the current kernel (raw 3,815,852 B, fits the 4M boot slot).
  U-Boot image is reused from the one-time SDK build.
