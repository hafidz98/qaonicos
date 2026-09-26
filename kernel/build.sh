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

$TOOL $COMMON $LDOPT -T kernel/virt.ld -o kernel/mach-kernel.elf \
    /tmp/mach_sched.o \
    kernel/start.S kernel/kernel_main.c \
    $B/vectors.S $B/trap.c $B/pmap.c $B/fpu.c $B/zone.c $B/ipc.c \
    $B/syscall.c $B/lib.c $B/gic.c $B/timer.c $B/vm.c \
    $LIBGCC

echo "built kernel/mach-kernel.elf"
