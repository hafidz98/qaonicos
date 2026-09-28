#!/bin/bash
# build-md.sh -- build ARM machine-dependent sources and link mach3.elf.
#
# 1. Runs build-mi.sh (MI objects -> build/obj/, now with ARM MD headers).
# 2. Compiles kernel/arm/*.c and *.s -> build/obj-md/.
# 3. Links with mach3.ld -> build/mach3.elf.
set -u

# Portable toolchain (survive VM reset): prefer ~/workspace/toolchain.
if [ -f "$HOME/workspace/toolchain/env.sh" ]; then
    # shellcheck disable=SC1091
    . "$HOME/workspace/toolchain/env.sh"
fi

MACH3_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${MACH3_SRC:-$HOME/workspace/mach3-src}/kernel"
BUILD="$MACH3_DIR/build"
GEN="$BUILD/gen"
INC="$BUILD/inc"
OBJ="$BUILD/obj"
OBJMD="$BUILD/obj-md"
LOG="$BUILD/md-compile.log"

if ! command -v clang >/dev/null 2>&1; then
    echo "clang missing, installing (standing authority 2026-09-27)..." >&2
    sudo apt-get install -y clang >/dev/null 2>&1 || \
        sudo apt-get install -y clang --no-install-recommends >&2
fi
command -v clang >/dev/null 2>&1 || { echo "FATAL: clang unavailable"; exit 1; }

# --- 1. MI ---
"$MACH3_DIR/build-mi.sh" || exit 1

# --- 1b. user programs (Fase B): init -> embedded image ---
# init.c + ulib: compile, link di INIT_CODE_VA (0x100000), objcopy ke
# biner, embed.py -> build/gen/init_img.c (simbol init_img/init_img_len).
# hello.c: cek kompilasi/link saja (biner tidak di-embed).
USR="$MACH3_DIR/../user"
UCFLAGS="--target=arm-none-eabi -march=armv7-a -O1 -fno-builtin -ffreestanding -I$USR"
echo "BUILD user/init + user/hello"
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/init.c" -o "$BUILD/init.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/init.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/ulib/ulib.c" -o "$BUILD/ulib.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/ulib/ulib.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/hello.c" -o "$BUILD/hello.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/hello.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/init.elf" "$BUILD/init.o" "$BUILD/ulib.o" >>"$LOG" 2>&1 || \
    { echo "FAIL link user/init.elf (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/hello.ld" \
    -o "$BUILD/hello.elf" "$BUILD/hello.o" "$BUILD/ulib.o" >>"$LOG" 2>&1 || \
    { echo "FAIL link user/hello.elf (see $LOG)"; exit 1; }
ENTRY=$(llvm-readelf-18 -h "$BUILD/init.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: init entry $ENTRY != 0x100000"; exit 1; }
llvm-objcopy-18 -O binary "$BUILD/init.elf" "$BUILD/init.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/init.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/init.bin" "$GEN/init_img.c" init_img 32768 >>"$LOG" 2>&1 || \
    { echo "FAIL embed init_img"; exit 1; }
echo "USER: init.elf entry=$ENTRY, hello.elf link OK"

# --- 2. MD compile ---
mkdir -p "$OBJMD"
: > "$LOG"

CFLAGS="--target=arm-none-eabi -march=armv7-a -DKERNEL -O1 -fno-builtin -fcommon"
OVR="$MACH3_DIR/mi-overrides"
CFLAGS="$CFLAGS -I$OVR -I$GEN -I$INC -I$SRC"
CFLAGS="$CFLAGS -Wno-implicit-function-declaration -Wno-implicit-int"
CFLAGS="$CFLAGS -Wno-int-conversion -Wno-deprecated-non-prototype"
CFLAGS="$CFLAGS -Wno-extra-tokens -include $GEN/md_globals.h"

pass=0; fail=0
for src in "$MACH3_DIR/kernel/arm/"*.c; do
    base=$(basename "$src" .c)
    echo "CC md/$base.c" >> "$LOG"
    # shellcheck disable=SC2086
    if clang $CFLAGS -c "$src" -o "$OBJMD/$base.o" >> "$LOG" 2>&1; then
        pass=$((pass+1))
    else
        echo "FAIL md/$base.c (see $LOG)"
        fail=$((fail+1))
    fi
done
for src in "$MACH3_DIR/kernel/arm/"*.s; do
    base=$(basename "$src" .s)
    echo "AS md/$base.s" >> "$LOG"
    # shellcheck disable=SC2086
    if clang --target=arm-none-eabi -march=armv7-a -c "$src" -o "$OBJMD/$base.o" >> "$LOG" 2>&1; then
        pass=$((pass+1))
    else
        echo "FAIL md/$base.s (see $LOG)"
        fail=$((fail+1))
    fi
done
echo "MD: $pass ok, $fail failed"
[ "$fail" -ne 0 ] && exit 1

# Fase B: compile generated init_img.c -> obj-md (embedded init program).
# shellcheck disable=SC2086
clang $CFLAGS -c "$GEN/init_img.c" -o "$OBJMD/init_img.o" >>"$LOG" 2>&1 || \
    { echo "FAIL md/init_img.c (see $LOG)"; exit 1; }
echo "MD: init_img.o ok"

# --- 3. link ---
MI_OBJS=$(find "$OBJ" -name '*.o' | sort)
MD_OBJS=$(find "$OBJMD" -name '*.o' | sort)
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$MACH3_DIR/mach3.ld" \
    -Wl,--allow-multiple-definition -o "$BUILD/mach3.elf" $MD_OBJS $MI_OBJS 2>&1 | tee -a "$LOG"

if [ -f "$BUILD/mach3.elf" ]; then
    echo "LINK OK: $BUILD/mach3.elf"
    clang --target=arm-none-eabi -nostdlib --print-targets >/dev/null 2>&1
    # size summary via llvm tools if present
    command -v llvm-objdump >/dev/null 2>&1 && \
        llvm-objdump -h "$BUILD/mach3.elf" | head -12
else
    echo "LINK FAILED (see $LOG)"
    exit 1
fi
