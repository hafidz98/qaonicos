#!/bin/sh
# build.sh - build the integrated Mach-x-Luckfox kernel for QEMU virt.
# Compiler: clang (bare-metal ARM). Usage: ./build.sh
set -e
cd "$(dirname "$0")/.."

TOOL="clang --target=arm-none-eabi"
LLVM_BIN=/usr/lib/llvm-18/bin
OBJCOPY=$LLVM_BIN/llvm-objcopy
OBJDUMP=$LLVM_BIN/llvm-objdump
READELF=$LLVM_BIN/llvm-readelf
# Linker: lld via clang driver (--ld-path eksplisit, bukan arm-none-eabi-ld).
LDOPT="--ld-path=/usr/bin/ld.lld"
COMMON="-mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=softfp \
-ffreestanding -nostdlib -O2 -Wall -Wextra -Werror"
B=rv1103-bringup

# __aeabi_* (divisi 64-bit dkk) disediakan sendiri di $B/aeabi.S + aeabi.c
# karena tidak ada compiler-rt baremetal untuk target ini.
# Clang tidak punya -mgeneral-regs-only; sched.c ditulis bebas-VFP dan
# diverifikasi lewat objdump di bawah.
$TOOL $COMMON -c $B/sched.c -o /tmp/mach_sched.o
if $OBJDUMP -d /tmp/mach_sched.o | grep -Eq "vstm|vldm|vpush|vpop|vmrs|vmsr|vadd|vmul|vmov\.f|vcvt"; then
    echo "FATAL: sched.o uses VFP instructions" >&2
    exit 1
fi

# Fase 8: program userspace pertama. Di-link di USER_PROG_VA tetap
# (0x10030000, _start di awal via .text.start), lalu di-embed sebagai
# blob biner -> array C hello_img (bukan objcopy -I binary: embed.py
# memberi nama simbol rapi + cek ukuran eksplisit).
$TOOL $COMMON -T user/hello.ld -o /tmp/mach_hello.elf user/hello.c
ENTRY=$($READELF -h /tmp/mach_hello.elf | sed -n 's/.*Entry point address: *//p')
if [ "$ENTRY" != "0x10030000" ]; then
    echo "FATAL: hello entry point $ENTRY != 0x10030000" >&2
    exit 1
fi
$OBJCOPY -O binary /tmp/mach_hello.elf /tmp/mach_hello.bin
python3 user/embed.py /tmp/mach_hello.bin /tmp/mach_hello_img.c hello_img 16384
$TOOL $COMMON -c /tmp/mach_hello_img.c -o /tmp/mach_hello_img.o

# Fase 9: program uji ramfs. Di-link di FSTEST_PROG_VA tetap
# (0x10028000, _start di awal via .text.start), lalu di-embed sebagai
# blob biner -> array C fstest_img (pola yang sama dengan hello).
$TOOL $COMMON -T user/fstest.ld -o /tmp/mach_fstest.elf user/fstest.c
ENTRY=$($READELF -h /tmp/mach_fstest.elf | sed -n 's/.*Entry point address: *//p')
if [ "$ENTRY" != "0x10028000" ]; then
    echo "FATAL: fstest entry point $ENTRY != 0x10028000" >&2
    exit 1
fi
$OBJCOPY -O binary /tmp/mach_fstest.elf /tmp/mach_fstest.bin
python3 user/embed.py /tmp/mach_fstest.bin /tmp/mach_fstest_img.c fstest_img 16384
$TOOL $COMMON -c /tmp/mach_fstest_img.c -o /tmp/mach_fstest_img.o

# Fase 10: init userspace + utilitas (ucat/uls/uecho). Di-link di VA
# tetap masing-masing (_start di awal via .text.start), ulib.c di-link
# ke dalam tiap biner (self-contained, pola hello/fstest), lalu
# di-embed sebagai blob -> array C <prog>_img. Entry point dicek
# terhadap konstanta VA di rv1103-bringup/user.h.
for prog in init ucat uls uecho; do
    case $prog in
        init)  VA=0x10012000 ;;
        ucat)  VA=0x10018000 ;;
        uls)   VA=0x10021000 ;;
        uecho) VA=0x10040000 ;;
    esac
    $TOOL $COMMON -T user/$prog.ld -o /tmp/mach_$prog.elf \
        user/$prog.c user/ulib.c
    ENTRY=$($READELF -h /tmp/mach_$prog.elf | sed -n 's/.*Entry point address: *//p')
    if [ "$ENTRY" != "$VA" ]; then
        echo "FATAL: $prog entry point $ENTRY != $VA" >&2
        exit 1
    fi
    $OBJCOPY -O binary /tmp/mach_$prog.elf /tmp/mach_$prog.bin
    python3 user/embed.py /tmp/mach_$prog.bin /tmp/mach_${prog}_img.c \
        ${prog}_img 16384
    $TOOL $COMMON -c /tmp/mach_${prog}_img.c -o /tmp/mach_${prog}_img.o
done

$TOOL $COMMON $LDOPT -T kernel/virt.ld -o kernel/mach-kernel.elf \
    /tmp/mach_sched.o /tmp/mach_hello_img.o /tmp/mach_fstest_img.o \
    /tmp/mach_init_img.o /tmp/mach_ucat_img.o /tmp/mach_uls_img.o \
    /tmp/mach_uecho_img.o \
    kernel/start.S kernel/kernel_main.c \
    $B/vectors.S $B/trap.c $B/pmap.c $B/fpu.c $B/zone.c $B/ipc.c \
    $B/syscall.c $B/lib.c $B/gic.c $B/timer.c $B/vm.c $B/task.c \
    $B/pager.c $B/user.c $B/fs.c $B/net.c $B/netstack.c $B/aeabi.S $B/aeabi.c

echo "built kernel/mach-kernel.elf (clang)"
