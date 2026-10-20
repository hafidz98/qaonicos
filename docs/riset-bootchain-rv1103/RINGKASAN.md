# Research: Rockchip RV1103 Boot Chain (B0 Phase)

Date: 2026-10-20. Goal: define the concrete path from QaonicOS kernel ELF
to a booting Luckfox Pico Mini B (RV1103, 128MB SPI NAND).

Sources: rockchip-linux/rkbin (verified file listing), Luckfox SDK
(luckfox-pico, via DeepWiki), Rockchip boot docs, Firefly wiki
(update.img pack/unpack), CNX Software (Mini A/B specs).

---

## 1. Boot Flow (RV1103B)

```
Power-on
  |
  v
MaskROM (BootROM in silicon, unchangeable)
  |  Searches for IDB at sector 64 (byte 0x8000) on boot media.
  |  If none found -> USB MaskROM download mode.
  v
idbloader (at sector 64)
  |  = DDR init blob + SPL (Secondary Program Loader)
  |  DDR: brings up 64MB DDR2. SPL: minimal HW init, loads U-Boot.
  v
U-Boot (from "uboot" partition)
  |  Reads parameter.txt (partition table), loads kernel.
  v
Kernel (from "boot" partition, boot.img format)
```

**Critical:** The BootROM is hardcoded to look at **sector 64** (LBA 64,
byte offset 0x8000). Sectors 0-63 are reserved for partition table
(GPT/MBR). This is true for all Rockchip SoCs.

---

## 2. idbloader: What It Is and Where It Comes From

The idbloader is **closed-source Rockchip binaries**. You cannot build
your own DDR initialization — DDR calibration is chip-specific and
Rockchip does not release source.

**Source:** `github.com/rockchip-linux/rkbin`, directory `bin/rv11/`:

| File | Purpose | Version |
|------|---------|---------|
| `rv1103b_ddr_924MHz_v1.07.bin` | DDR2 init (924 MHz) | v1.07 |
| `rv1103b_ddr_924MHz_tb_v1.07.bin` | DDR init, TB variant (for SPI NAND) | v1.07 |
| `rv1103b_spl_v1.01.bin` | SPL (Secondary Program Loader) | v1.01 |
| `rv1103b_usbplug_v1.00.bin` | USB download mode handler | v1.00 |
| `rv1103b_usbplug_spinand_cont_v1.10.bin` | USB plug for SPI NAND | v1.10 |
| `rv1103b_flash_acc_v1.10.bin` | Flash access helper | v1.10 |
| `rv1103b_hpmcu_tb_v1.00.bin` | HPMCU firmware (RISC-V helper) | v1.00 |

**Assembly config:** `RKBOOT/RV1103BMINIALL.ini` (standard) or
`RKBOOT/RV1103BMINIALL_SPI_NAND_CONT_TB.ini` (SPI NAND):

```ini
[LOADER_OPTION]
NUM=2
LOADER1=FlashData
LOADER2=FlashBoot
FlashData=bin/rv11/rv1103b_ddr_924MHz_v1.07.bin
FlashBoot=bin/rv11/rv1103b_spl_v1.01.bin
[OUTPUT]
PATH=rv1103b_download_v1.07.101.bin
IDB_PATH=rv1103b_idblock_v1.07.101.img
```

**Tool:** Rockchip `boot_merger` (in rkbin `tools/` or Luckfox SDK)
reads the .ini and produces:
- `*_download_*.bin` — for USB flashing (contains usbplug)
- `*_idblock_*.img` — the **idbloader.img**, written to sector 64

**Key decision for B0:** **Reuse Rockchip's idbloader as-is.**
We do NOT need to build or modify it. DDR init is the hardest part
and Rockchip provides it. We only replace U-Boot onwards.

---

## 3. update.img Format

`update.img` is Rockchip's firmware package format. It is NOT a raw
disk image — it is a container with a manifest.

**Structure:**
```
update.img
  ├── package-file          (manifest: file list + offsets)
  └── Image/
      ├── MiniLoaderAll.bin (DDR + usbplug + SPL, for flashing)
      ├── parameter.txt     (partition table)
      ├── uboot.img         (U-Boot)
      ├── trust.img         (ARM Trusted Firmware, if used)
      ├── boot.img          (kernel + dtb)
      ├── rootfs.img        (root filesystem)
      └── ... (oem.img, userdata.img, etc.)
```

**Tools:**
- `afptool` — pack/unpack update.img (from
  `github.com/TeeFirefly/rk2918_tools`)
  - Unpack: `afptool -unpack update.img update/`
  - Pack: `afptool -pack update/ new_update.img`
- `img_unpack` / `img_maker` — handle the outer `release_update.img`
  wrapper (some vendors add this)

**parameter.txt** defines the partition layout. Example for
SPI NAND (from Luckfox SDK):
```
256K(env),256K@256K(idblock),512K(uboot),4M(boot),30M(oem),6M(userdata),85M(rootfs)
```

---

## 4. Flashing Tools and Procedure (Mini B)

**Enter MaskROM mode:** Hold the **BOOT button** while plugging in USB.
The board appears as a Rockchip USB device (VID 0x2207).

**Tools:**

| Tool | Platform | Notes |
|------|----------|-------|
| SocToolKit | Windows | From Luckfox SDK (`Tools/windows/SocToolKit/`). GUI. Select chip RV1103. |
| `upgrade_tool` | Linux | Closed-source CLI from Rockchip. **Only supports SPI NAND and eMMC** (not SD). Ubuntu 22.04 x86_64. |
| `rkdeveloptool` | Linux | Open source. Commands: `db` (download boot), `ul` (upload loader), `wl` (write LBA), `rd` (reset). |

**Flashing update.img (Linux):**
```bash
sudo upgrade_tool uf update.img
```

**Flashing individual partitions (rkdeveloptool):**
```bash
# Download loader to RAM (for communication)
sudo rkdeveloptool db rv1103b_download_v1.07.101.bin
# Write idbloader to sector 64
sudo rkdeveloptool wl 64 idbloader.img
# Write U-Boot
sudo rkdeveloptool wl 16384 uboot.img
# Write boot image
sudo rkdeveloptool wl 32768 boot.img
# Reboot
sudo rkdeveloptool rd
```

---

## 5. Mini A vs Mini B Boot Differences

| Feature | Mini A | Mini B |
|---------|--------|--------|
| SoC | RV1103 | RV1103 |
| RAM | 64MB DDR2 | 64MB DDR2 |
| Primary storage | MicroSD slot | 128MB SPI NAND (Winbond W25N01GV) |
| Secondary storage | — | MicroSD slot (also present!) |
| NAND pads | Unpopulated pads | Populated |

**Critical for development:** The Mini B has **BOTH** SPI NAND and a
TF card slot. The BootROM tries boot media in a priority order. If a
valid idbloader is found on the SD card, it will boot from SD —
even if the NAND has a bootloader.

**This means:** For B0 development, we can **boot from SD card**
without touching the NAND. Write idbloader to sector 64 of the SD,
put U-Boot and our kernel on the SD. This is non-destructive and
fast to iterate.

**For production:** Flash to SPI NAND via `update.img` + `upgrade_tool`.

**SD card boot image creation (SocToolKit):**
1. Format SD card
2. SocToolKit → SDTool mode → Select "SD Card Boot"
3. Import boot files (idbloader, uboot, boot — NOT update.img)
4. Click "Create SD Card"

---

## 6. U-Boot and Kernel Loading

**U-Boot source:** Luckfox SDK builds U-Boot from Rockchip's fork.
Config fragment for SPI NAND: `rk-sfc.config`.
Config fragment for SD: `rk-emmc.config`.

**How U-Boot loads the kernel:**
1. U-Boot reads `parameter.txt` to find the `boot` partition.
2. Loads `boot.img` from that partition into RAM.
3. `boot.img` is in **Android boot image format** (magic `"ANDROID!"`):
   - Header (2KB): kernel_size, kernel_addr, ramdisk_size, etc.
   - Kernel binary
   - Ramdisk (optional)
   - DTB (device tree)
4. U-Boot jumps to `kernel_addr`.

**Tool to create boot.img:** `mkkrnlimg` or `mkbootimg` (from
rk2918_tools or Android build tools).

**For QaonicOS:** Our kernel is a raw ARM binary (not Linux).
Options:
- **Option A (simplest):** Use U-Boot's `go` command to jump to a
  raw binary loaded via `fatload` (from SD) or `nand read`.
  ```
  fatload mmc 0:1 0x40000000 qaonic_kernel.bin
  go 0x40000000
  ```
- **Option B:** Package our kernel as a minimal `boot.img`
  (with a dummy header) so the standard boot flow works.
- **Option C (long-term):** Replace U-Boot entirely with our own
  second-stage loader. Not recommended for B0 — reuse U-Boot.

**Recommendation for B0:** Option A. Keep Rockchip's idbloader +
Luckfox's U-Boot. Load our kernel via U-Boot `go`. This minimizes
risk and lets us focus on the kernel itself.

---

## 7. Concrete Steps: Kernel ELF to Booting Mini B

### Phase B0a: SD Card Boot (Development, Non-Destructive)

**Prerequisites (one-time setup):**
1. Clone `rockchip-linux/rkbin`
2. Get `boot_merger` tool (from rkbin `tools/` or Luckfox SDK)
3. Get Luckfox SDK U-Boot binary (or build from SDK)
   - Or: extract `uboot.img` from a Luckfox firmware `update.img`
     using `afptool -unpack`

**Steps:**
1. **Create idbloader:**
   ```bash
   # Using boot_merger with RV1103BMINIALL.ini
   boot_merger RKBOOT/RV1103BMINIALL.ini
   # Output: rv1103b_idblock_v1.07.101.img
   ```

2. **Prepare SD card:**
   ```bash
   # Write idbloader to sector 64
   sudo dd if=rv1103b_idblock_v1.07.101.img of=/dev/sdX seek=64
   # Create FAT partition for kernel (starting at sector 8192)
   # Copy qaonic_kernel.bin to the FAT partition
   ```

3. **Boot and load kernel via U-Boot:**
   - Insert SD, power on (BootROM loads idbloader from SD)
   - Interrupt U-Boot (press key during countdown)
   - ```
     fatload mmc 0:1 0x40000000 qaonic_kernel.bin
     go 0x40000000
     ```

4. **Automate:** Set U-Boot `bootcmd` to auto-load:
   ```
   setenv bootcmd 'fatload mmc 0:1 0x40000000 qaonic_kernel.bin; go 0x40000000'
   saveenv
   ```

### Phase B0b: SPI NAND Flash (Production)

1. **Create boot.img** with our kernel:
   ```bash
   # Option A: raw binary in a partition
   # Just place qaonic_kernel.bin as a file in the boot partition
   ```

2. **Create update.img:**
   ```bash
   # Unpack a reference Luckfox update.img
   afptool -unpack luckfox_update.img ref/
   # Replace Image/boot.img with our kernel
   # (or add our kernel as a new partition image)
   cp qaonic_kernel.bin ref/Image/qaonic.img
   # Edit ref/package-file to include it
   # Repack
   afptool -pack ref/ qaonic_update.img
   ```

3. **Flash:**
   ```bash
   # Hold BOOT button, plug USB
   sudo upgrade_tool uf qaonic_update.img
   ```

### Files Needed from Luckfox SDK

| File | Location in SDK | Purpose |
|------|----------------|---------|
| `MiniLoaderAll.bin` | `output/image/` | For USB flashing |
| `uboot.img` | `output/image/` | U-Boot bootloader |
| `parameter.txt` | `output/image/` | Partition layout |
| `boot_merger` | `tools/` | Assemble idbloader |
| SocToolKit | `Tools/windows/` | Windows flashing GUI |
| `upgrade_tool` | `tools/linux/` | Linux flashing CLI |

**Shortcut:** If building the full SDK is too heavy, extract these
from a prebuilt Luckfox firmware `update.img` using `afptool`.

---

## 8. Open Questions for B0 Implementation

1. **U-Boot `go` command:** Does Luckfox's U-Boot build include the
   `go` command? (Usually yes, but verify.)
2. **Kernel load address:** Our kernel currently links for QEMU
   (`0x40000000`?). The RV1103 has DDR2 at a specific physical
   address — need to confirm it matches. (RV1103 TRM needed.)
3. **UART for debug:** Which UART is the debug console on Mini B?
   (Likely UART2, per earlier research. Confirm via schematic.)
4. **SD boot priority:** Confirm that Mini B BootROM prefers SD
   over NAND when both have valid idbloaders. (Test empirically.)

---

## 9. SD Card Support

The user specifically asked about SD card support. Here is the full
picture:

### Hardware
- **Mini A:** MicroSD slot is the PRIMARY and ONLY storage.
- **Mini B:** MicroSD slot is present AND functional, in addition
  to the 128MB SPI NAND. Both can be used.

### QaonicOS Current SD Support
- **QEMU:** SD card works via `virtio-blk-device` (emulated).
  FAT32 driver (`fat32.c`) reads/writes. Tested: `ufs` PASS.
- **Hardware:** The SD controller on RV1103 is `sdmmc`. Our kernel
  does NOT yet have an SDMMC driver for real hardware. The QEMU
  virtio-blk is completely different from the real SDHCI controller.

### What's Needed for Real SD on RV1103
1. **SDMMC driver:** Initialize the SDHCI controller, send CMD0/CMD8/
   ACMD41 to identify the card, set bus width and clock.
   Base address: check RV1103 TRM (likely `0xFF7C0000` for SDMMC0,
   but VERIFY — this is a guess).
2. **Reuse FAT32:** Once block read/write works, our existing
   `fat32.c` works unchanged (it uses `sd_read`/`sd_write` backend).
3. **U-Boot already handles SD:** For B0a (SD boot via U-Boot `go`),
   we don't need our own SD driver initially — U-Boot loads the
   kernel. Our SD driver is needed later for filesystem access.

### Recommendation
- **B0:** Rely on U-Boot for SD access (loading kernel). No SD
  driver needed in QaonicOS yet.
- **B1:** Implement SDMMC driver for FAT32 filesystem access
  (alongside SPI NAND driver).

---

## Summary

| Question | Answer |
|----------|--------|
| Boot flow | MaskROM → idbloader (sector 64) → U-Boot → kernel |
| idbloader source | Rockchip rkbin (closed binary), reuse as-is |
| DDR init | `rv1103b_ddr_924MHz_v1.07.bin`, cannot build our own |
| update.img | Rockchip container, pack with `afptool` |
| Flash tool (Linux) | `upgrade_tool` (SPI NAND), `rkdeveloptool` (open) |
| Flash tool (Windows) | SocToolKit |
| Mini B SD boot | Yes, supported — use for development |
| Kernel loading | U-Boot `go` command (simplest for B0) |
| Files from SDK | MiniLoaderAll.bin, uboot.img, parameter.txt, boot_merger |
| SD driver in QaonicOS | Not yet (QEMU only); needed for B1 |
