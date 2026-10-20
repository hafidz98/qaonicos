# Flashing Guide: QaonicOS on Luckfox Pico Mini B

Date: 2026-10-20. Board: Luckfox Pico Mini B (RV1103B, 128MB SPI NAND).
Image: `update-final.img` (built per PLAN-B0.md).

## What you need
- Luckfox Pico Mini B board
- USB-C cable (data, not charge-only)
- Computer with the flash tool:
  - Linux: `upgrade_tool` (in SDK `tools/linux/Linux_Upgrade_Tool/`)
  - Windows: SocToolKit (in SDK `tools/windows/`)

## Step 1: Enter MaskROM mode
1. Disconnect the board.
2. **Press and hold the BOOT button** on the board.
3. While holding BOOT, plug in the USB-C cable.
4. Release BOOT after ~2 seconds.

The board now enumerates as a Rockchip USB device (VID 0x2207).
MaskROM lives in the chip itself — this always works, even if the
NAND is empty or corrupted. **The board cannot be bricked.**

Check (Linux): `lsusb | grep 2207`

## Step 2: Flash
Linux:
```
upgrade_tool uf update-final.img
```
Wait for "Download firmware OK".

Windows (SocToolKit):
1. Select chip: RV1103.
2. Load `update-final.img`.
3. Click Run/Start; wait for success.

## Step 3: Boot
1. Unplug USB, plug it back in (without holding BOOT).
2. The board boots: MaskROM -> idbloader (DDR init) -> U-Boot ->
   QaonicOS kernel.
3. Connect a serial console to the debug UART to see the boot log
   and the `qaon>` prompt.

## Recovery (if anything goes wrong)
Repeat Step 1 + Step 2. MaskROM mode ignores whatever is in NAND,
so a bad image can always be replaced.

## Image contents (for reference)
| Partition | Content |
|---|---|
| idblock | idbloader (DDR init + SPL) from Rockchip rkbin |
| uboot | U-Boot built from Luckfox SDK source |
| boot | QaonicOS kernel (Mach 3 + shell + face + qabot) |
| rootfs | placeholder (QaonicOS embeds its files in the kernel) |

Partition layout:
```
256K(env), 256K(idblock), 512K(uboot), 4M(boot),
30M(oem), 6M(userdata), 85M(rootfs)
```

## Known unknowns (first hardware test will tell)
- Whether U-Boot loads our raw kernel from the boot partition
  directly, or expects Android boot.img format. If it refuses,
  wrap the kernel with mkbootimg or use U-Boot `go` interactively.
- Debug UART pin assignment on the Mini B.
- Whether the kernel (linked for QEMU addresses) runs unmodified
  at the hardware load address.
