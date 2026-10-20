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

## 2. Boot methods (two paths)

### 2.1 SD card boot (DEVELOPMENT — primary)
The Mini B has BOTH 128MB SPI NAND and a TF (microSD) slot.
BootROM checks SD for a valid idbloader — **Mini B can boot from SD**.

Flow:
1. Create bootable SD (SocToolKit "SD Card Boot" mode, or dd the
   idbloader + U-Boot + kernel to the right offsets).
2. Insert SD, power on. BootROM loads idbloader from SD.
3. U-Boot loads `qaonic_kernel.bin` from SD FAT partition.
4. Kernel runs. NAND untouched — non-destructive, fast iteration.

This is the recommended dev loop: no flashing, swap SD to test.

### 2.2 NAND boot (PRODUCTION)
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

### 4.1 Current state
- QEMU: SD works via virtio-blk (`blk.c` dev 1), FAT32 on top
  (`fat32.c`). Test `ufs` PASS. Shell `ls`/`cat` work on `/sd`.
- Hardware: **no SDMMC driver yet.** The RV1103 uses a DesignWare
  MSHC controller at `0xffaa0000` (from `docs/hw-addrs.md`) —
  completely different from virtio-blk.

### 4.2 What B0 needs vs what B1 needs
- **B0 (boot)**: U-Boot loads the kernel from SD. The kernel does
  NOT need an SD driver to boot. SD-as-storage can wait.
- **B1 (storage)**: Implement SDMMC (DW_mshc) driver so the running
  OS can read/write the SD card. The existing `fat32.c` then works
  unchanged (it only calls `sd_read`/`sd_write`).

### 4.3 How the user uses the SD card (target UX)
Once B1 lands, the SD card is the user's removable storage:
- **Insert card** -> auto-detected, mounted at `/sd`.
- **Shell**: `ls /sd`, `cat /sd/file.txt` (already works in QEMU).
- **Data**: user files, logs, audio recordings live on SD.
- **Boot**: card with idbloader+kernel boots the board (dev mode);
  a plain FAT card is just storage.
- **Format**: FAT32 (interoperable with PC — copy files by plugging
  the card into a computer).

## 5. B0 tasks

- [ ] B0.1 Obtain SDK files: MiniLoaderAll.bin, uboot.img,
      parameter.txt, boot_merger, upgrade_tool/SocToolKit.
      (Shortcut: `afptool -unpack` a stock firmware image.)
- [ ] B0.2 Build `qaonic_kernel.bin` from mach3.elf (objcopy -O binary).
- [ ] B0.3 Create bootable SD; test U-Boot `fatload` + `go`.
- [ ] B0.4 Resolve Q1-Q4 (U-Boot `go`, load address, UART pins,
      boot priority).
- [ ] B0.5 Package `update.img` (afptool) for NAND flashing.
- [ ] B0.6 Write flashing guide (docs) for both SD and NAND paths.
- [ ] B0.7 Hardware test entries -> `docs/rencana-uji-hardware/`
      (H0 extension: boot from SD, boot from NAND, maskrom recovery).

## 6. Acceptance criteria
- Board boots from SD to `qaon>` prompt with no manual steps
  beyond inserting the card and powering on.
- Board boots from NAND after flashing `update.img`.
- Maskrom recovery still works after a bad flash (unbrickable).
- Kernel banner + logo show on serial console.

## 7. Out of scope for B0 (later phases)
- SDMMC driver in the OS (B1).
- littlefs on NAND (B1).
- Watchdog/boot counter (B2).
- Provisioning (B3).
- A/B updates (B4).
