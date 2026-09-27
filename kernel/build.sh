#!/bin/sh
# build.sh - build the integrated Mach-x-Luckfox kernel for QEMU virt.
# Usage: ./build.sh [gcc|clang]
set -e
cd "$(dirname "$0")/.."

CC="${1:-gcc}"
COMMON="-mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=softfp \
-ffreestanding -nostdlib -O2 -Wall -Wextra -Werror"
B=rv1103-bringup

if [ "$CC" = "clang" ]; then
    TOOL="clang --target=arm-none-eabi"
    LDOPT="--ld-path=/usr/bin/arm-none-eabi-ld"
    LIBGCC=""
    # Clang has no -mgeneral-regs-only; sched.c is written VFP-free and
    # verified so with objdump below.
    SCHEDFLAGS=""
    # No compiler-rt for this target here; reuse GCC's thumb libgcc
    # (plain archive, compiler-agnostic) for __aeabi_* helpers.
    LIBGCC="$(arm-none-eabi-gcc -mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=softfp -print-libgcc-file-name)"
else
    TOOL="arm-none-eabi-gcc"
    LDOPT=""
    # This Ubuntu toolchain only ships a thumb multilib libgcc; link it
    # explicitly (interworking veneers handle ARM->thumb calls). Needed
    # for 64-bit division helpers used by timer.c.
    LIBGCC="$($TOOL -mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=softfp -print-libgcc-file-name)"
    SCHEDFLAGS="-mgeneral-regs-only"
fi

# sched.c must not touch VFP: the IRQ path's eager fpu_save/restore has to
# capture the threads' real VFP registers, not the compiler's temporaries.
$TOOL $COMMON $SCHEDFLAGS -c $B/sched.c -o /tmp/mach_sched.o
if arm-none-eabi-objdump -d /tmp/mach_sched.o | grep -Eq "vstm|vldm|vpush|vpop|vmrs|vmsr|vadd|vmul|vmov\.f|vcvt"; then
    echo "FATAL: sched.o uses VFP instructions" >&2
    exit 1
fi

# Fase 8: program userspace pertama. Di-link di USER_PROG_VA tetap
# (0x10030000, _start di awal via .text.start), lalu di-embed sebagai
# blob biner -> array C hello_img (bukan objcopy -I binary: embed.py
# memberi nama simbol rapi + cek ukuran eksplisit).
$TOOL $COMMON -T user/hello.ld -o /tmp/mach_hello.elf user/hello.c
ENTRY=$(arm-none-eabi-readelf -h /tmp/mach_hello.elf | sed -n 's/.*Entry point address: *//p')
if [ "$ENTRY" != "0x10030000" ]; then
    echo "FATAL: hello entry point $ENTRY != 0x10030000" >&2
    exit 1
fi
arm-none-eabi-objcopy -O binary /tmp/mach_hello.elf /tmp/mach_hello.bin
python3 user/embed.py /tmp/mach_hello.bin /tmp/mach_hello_img.c hello_img 16384
$TOOL $COMMON -c /tmp/mach_hello_img.c -o /tmp/mach_hello_img.o

# Fase 9: program uji ramfs. Di-link di FSTEST_PROG_VA tetap
# (0x10028000, _start di awal via .text.start), lalu di-embed sebagai
# blob biner -> array C fstest_img (pola yang sama dengan hello).
$TOOL $COMMON -T user/fstest.ld -o /tmp/mach_fstest.elf user/fstest.c
ENTRY=$(arm-none-eabi-readelf -h /tmp/mach_fstest.elf | sed -n 's/.*Entry point address: *//p')
if [ "$ENTRY" != "0x10028000" ]; then
    echo "FATAL: fstest entry point $ENTRY != 0x10028000" >&2
    exit 1
fi
arm-none-eabi-objcopy -O binary /tmp/mach_fstest.elf /tmp/mach_fstest.bin
python3 user/embed.py /tmp/mach_fstest.bin /tmp/mach_fstest_img.c fstest_img 16384
$TOOL $COMMON -c /tmp/mach_fstest_img.c -o /tmp/mach_fstest_img.o

$TOOL $COMMON $LDOPT -T kernel/virt.ld -o kernel/mach-kernel.elf \
    /tmp/mach_sched.o /tmp/mach_hello_img.o /tmp/mach_fstest_img.o \
    kernel/start.S kernel/kernel_main.c \
    $B/vectors.S $B/trap.c $B/pmap.c $B/fpu.c $B/zone.c $B/ipc.c \
    $B/syscall.c $B/lib.c $B/gic.c $B/timer.c $B/vm.c $B/task.c \
    $B/pager.c $B/user.c $B/fs.c \
    $LIBGCC

echo "built kernel/mach-kernel.elf"
