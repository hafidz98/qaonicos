#!/bin/bash
# Build + run bare-metal Cortex-A7 hello world di QEMU.
# Butuh: gcc-arm-none-eabi, qemu-system-arm
set -e
cd "$(dirname "$0")"

CC=${CC:-arm-none-eabi-gcc}
OBJCOPY=${OBJCOPY:-arm-none-eabi-objcopy}

echo "[*] compile..."
$CC -mcpu=cortex-a7 -marm -nostdlib -ffreestanding -O2 -Wall -c start.S -o start.o
$CC -mcpu=cortex-a7 -marm -nostdlib -ffreestanding -O2 -Wall -c main.c -o main.o
$CC -mcpu=cortex-a7 -marm -nostdlib -T link.ld start.o main.o -o hello.elf -lgcc
$OBJCOPY -O binary hello.elf hello.bin
echo "[*] build OK: hello.elf ($(stat -c%s hello.elf) bytes)"

if [ "$1" = "run" ]; then
    echo "[*] run di QEMU (Ctrl-A X untuk keluar)..."
    exec qemu-system-arm -M virt -cpu cortex-a7 -nographic -kernel hello.elf
fi
