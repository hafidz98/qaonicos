#!/bin/sh
# build-update-img.sh - Reproducible flashable image for the
# Luckfox Pico Mini B (RV1103B, 128 MB SPI NAND).
#
# Produces update-final.img from source in one run:
#   1. build the QaonicOS kernel (mach3-port/build-md.sh)
#   2. ELF -> raw binary (U-Boot loads raw images, not ELF)
#   3. idbloader.img via Rockchip rkbin boot_merger
#      (DDR init + usbplug + SPL, SPI NAND continuous mode)
#   4. assemble the Rockchip package tree (package-file manifest)
#   5. afptool -pack -> update.img
#   6. rkImageMaker wrap -> update-final.img (flashable)
#
# Prerequisites (one-time, see docs/rencana-bootchain/PLAN-B0.md):
#   - rkbin cloned at $B0_SDK/rkbin
#   - Luckfox SDK sparse checkout at $B0_SDK/luckfox-pico
#     (provides afptool + rkImageMaker)
#   - U-Boot image at $B0_SDK/uboot.img (built once from the SDK;
#     recipe in PLAN-B0.md -- this script reuses it)
#
# Env overrides:
#   B0_SDK=~/workspace/b0-sdk   location of rkbin/SDK/artifacts
#   OUT=$B0_SDK                 output directory
#   SKIP_KERNEL_BUILD=1         reuse mach3-port/build/mach3.elf
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
B0_SDK="${B0_SDK:-$HOME/workspace/b0-sdk}"
OUT="${OUT:-$B0_SDK}"
RKBIN="$B0_SDK/rkbin"
PACK_TOOLS="$B0_SDK/luckfox-pico/tools/linux/Linux_Pack_Firmware"
BOOT_PART_MAX=4194304	# 4M boot partition (parameter.txt)

source "$HOME/workspace/toolchain/env.sh" 2>/dev/null || true

for t in "$RKBIN/tools/boot_merger" "$PACK_TOOLS/afptool" \
    "$PACK_TOOLS/rkImageMaker" "$B0_SDK/uboot.img"; do
	if [ ! -e "$t" ]; then
		echo "MISSING prerequisite: $t" >&2
		echo "See docs/rencana-bootchain/PLAN-B0.md" >&2
		exit 1
	fi
done

mkdir -p "$OUT/pkg/Image"

echo "[1/6] kernel"
if [ -z "$SKIP_KERNEL_BUILD" ]; then
	(cd mach3-port && ./build-md.sh >/dev/null)
fi
[ -f mach3-port/build/mach3.elf ] || { echo "no mach3.elf" >&2; exit 1; }

echo "[2/6] kernel raw binary"
llvm-objcopy-18 -O binary mach3-port/build/mach3.elf "$OUT/qaonic_kernel.bin"
KSIZE=$(stat -c%s "$OUT/qaonic_kernel.bin")
echo "  qaonic_kernel.bin: $KSIZE bytes (boot partition $BOOT_PART_MAX)"
[ "$KSIZE" -lt "$BOOT_PART_MAX" ] || {
	echo "kernel too big for the 4M boot partition" >&2; exit 1; }

echo "[3/6] idbloader (rkbin boot_merger)"
(cd "$RKBIN" && ./tools/boot_merger \
	RKBOOT/RV1103BMINIALL_SPI_NAND_CONT_TB.ini >/dev/null)
# boot_merger writes a versioned name (rv1103b_idblock_v*.img).
IDB=$(ls -t "$RKBIN"/*_idblock_*.img | head -1)
cp "$IDB" "$OUT/idbloader.img"
echo "  idbloader.img: $(stat -c%s "$OUT/idbloader.img") bytes"

echo "[4/6] package tree"
cp "$OUT/idbloader.img" "$OUT/pkg/Image/download.bin"
cp scripts/update-img/parameter.txt "$OUT/pkg/Image/parameter.txt"
cp "$B0_SDK/uboot.img" "$OUT/pkg/Image/uboot.img"
cp "$OUT/qaonic_kernel.bin" "$OUT/pkg/Image/boot.img"
dd if=/dev/zero of="$OUT/pkg/Image/rootfs.img" bs=1M count=1 2>/dev/null
printf 'package-file\tpackage-file\nbootloader\tImage/download.bin\nparameter\tImage/parameter.txt\nuboot\tImage/uboot.img\nboot\tImage/boot.img\nrootfs\tImage/rootfs.img\n' \
	> "$OUT/pkg/package-file"

echo "[5/6] afptool pack"
(cd "$OUT/pkg" && "$PACK_TOOLS/afptool" -pack ./ "$OUT/update.img" >/dev/null)
echo "  update.img: $(stat -c%s "$OUT/update.img") bytes"

echo "[6/6] rkImageMaker wrap"
(cd "$OUT" && "$PACK_TOOLS/rkImageMaker" -RK1103 \
	pkg/Image/download.bin update.img update-final.img \
	-os_type:androidos >/dev/null)
echo "DONE: $OUT/update-final.img ($(stat -c%s "$OUT/update-final.img") bytes)"
sha256sum "$OUT/update-final.img"
