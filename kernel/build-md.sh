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

# --- 1b. user programs (Fase C): init/ucat/uls/uecho/umon -> embedded ---
# Tiap program: compile, link di INIT_CODE_VA (0x100000) via init.ld,
# objcopy ke biner, embed.py -> build/gen/<name>_img.c
# (simbol <name>_img/<name>_img_len).  umon + tui.c.
USR="$MACH3_DIR/../user"
UCFLAGS="--target=arm-none-eabi -march=armv7-a -O1 -fno-builtin -ffreestanding -I$USR"
echo "BUILD user programs (init ucat uls uecho umon ugpio usd ufs)"
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/ulib/ulib.c" -o "$BUILD/ulib.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/ulib/ulib.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/tui.c" -o "$BUILD/tui.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/tui.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/udiv.c" -o "$BUILD/udiv.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/udiv.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/hello.c" -o "$BUILD/hello.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/hello.c (see $LOG)"; exit 1; }
for prog in init ucat uls uecho umon ugpio usd ufs; do
    # shellcheck disable=SC2086
    clang $UCFLAGS -c "$USR/$prog.c" -o "$BUILD/$prog.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/$prog.c (see $LOG)"; exit 1; }
    extra=""
    [ "$prog" = "umon" ] && extra="$BUILD/tui.o"
    # shellcheck disable=SC2086
    clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
        -o "$BUILD/$prog.elf" "$BUILD/$prog.o" "$BUILD/ulib.o" \
        "$BUILD/udiv.o" $extra \
        >>"$LOG" 2>&1 || \
        { echo "FAIL link user/$prog.elf (see $LOG)"; exit 1; }
    ENTRY=$(llvm-readelf-18 -h "$BUILD/$prog.elf" | sed -n 's/.*Entry point address: *//p')
    [ "$ENTRY" = "0x100000" ] || \
        { echo "FATAL: $prog entry $ENTRY != 0x100000"; exit 1; }
    llvm-objcopy-18 -O binary "$BUILD/$prog.elf" "$BUILD/$prog.bin" >>"$LOG" 2>&1 || \
        { echo "FAIL objcopy user/$prog.bin"; exit 1; }
    # Fase D: objcopy -O binary tidak menyertakan .bss (NOBITS) di akhir;
    # pad binary dengan nol hingga akhir .bss agar img_len mencakup
    # footprint memori penuh (loader menghitung npages dari img_len).
    python3 "$USR/pad-bss.py" "$BUILD/$prog.bin" "$BUILD/$prog.elf" \
        >>"$LOG" 2>&1 || \
        { echo "FAIL pad-bss user/$prog.bin"; exit 1; }
    python3 "$USR/embed.py" "$BUILD/$prog.bin" "$GEN/${prog}_img.c" "${prog}_img" 32768 >>"$LOG" 2>&1 || \
        { echo "FAIL embed ${prog}_img"; exit 1; }
    echo "USER: $prog.elf entry=$ENTRY ok"
done
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/hello.ld" \
    -o "$BUILD/hello.elf" "$BUILD/hello.o" "$BUILD/ulib.o" >>"$LOG" 2>&1 || \
    { echo "FAIL link user/hello.elf (see $LOG)"; exit 1; }
echo "USER: hello.elf link OK"

# --- 1c. user program face/Qabot (App A1): multi-file (main.c + face.c +
# face_draw.c), pola sama: link di 0x100000, objcopy, pad-bss, embed. ---
echo "BUILD user program face (Qabot)"
FCFLAGS="$UCFLAGS -I$USR/face -Wno-unused-function"
for f in main face face_draw; do
    # shellcheck disable=SC2086
    clang $FCFLAGS -c "$USR/face/$f.c" -o "$BUILD/face_$f.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/face/$f.c (see $LOG)"; exit 1; }
done
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/face.elf" "$BUILD/face_main.o" "$BUILD/face_face.o" \
    "$BUILD/face_face_draw.o" "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/face.elf (see $LOG)"; exit 1; }
ENTRY=$(llvm-readelf-18 -h "$BUILD/face.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: face entry $ENTRY != 0x100000"; exit 1; }
llvm-objcopy-18 -O binary "$BUILD/face.elf" "$BUILD/face.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/face.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/face.bin" "$BUILD/face.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/face.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/face.bin" "$GEN/face_img.c" "face_img" 32768 >>"$LOG" 2>&1 || \
    { echo "FAIL embed face_img"; exit 1; }
echo "USER: face.elf entry=$ENTRY ok"

# --- 1d. user program uiapp (App A2/A3): multi-file (main.c + ui.c +
# ui_draw.c + scr_*.c + uartproto), pola sama: link di 0x100000, objcopy, pad-bss. ---
echo "BUILD user program uiapp"
UCFLAGS2="$UCFLAGS -I$USR/uiapp -I$USR/face -I$USR -Wno-unused-function"
for f in main ui ui_draw scr_menu scr_mon scr_settings scr_power scr_wifi scr_ble scr_llm scr_passkey scr_textedit; do
    # shellcheck disable=SC2086
    clang $UCFLAGS2 -c "$USR/uiapp/$f.c" -o "$BUILD/uiapp_$f.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/uiapp/$f.c (see $LOG)"; exit 1; }
done
for f in uartproto mock_comcu; do
    # shellcheck disable=SC2086
    clang $UCFLAGS2 -c "$USR/uartproto/$f.c" -o "$BUILD/uartproto_$f.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/uartproto/$f.c (see $LOG)"; exit 1; }
done
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/uiapp.elf" "$BUILD/uiapp_main.o" "$BUILD/uiapp_ui.o" \
    "$BUILD/uiapp_ui_draw.o" "$BUILD/uiapp_scr_menu.o" \
    "$BUILD/uiapp_scr_mon.o" "$BUILD/uiapp_scr_settings.o" \
    "$BUILD/uiapp_scr_power.o" "$BUILD/uiapp_scr_wifi.o" \
    "$BUILD/uiapp_scr_ble.o" "$BUILD/uiapp_scr_llm.o" \
    "$BUILD/uiapp_scr_passkey.o" "$BUILD/uiapp_scr_textedit.o" \
    "$BUILD/uartproto_uartproto.o" "$BUILD/uartproto_mock_comcu.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/uiapp.elf (see $LOG)"; exit 1; }
ENTRY=$(llvm-readelf-18 -h "$BUILD/uiapp.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: uiapp entry $ENTRY != 0x100000"; exit 1; }
llvm-objcopy-18 -O binary "$BUILD/uiapp.elf" "$BUILD/uiapp.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/uiapp.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/uiapp.bin" "$BUILD/uiapp.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/uiapp.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/uiapp.bin" "$GEN/uiapp_img.c" "uiapp_img" 65536 >>"$LOG" 2>&1 || \
    { echo "FAIL embed uiapp_img"; exit 1; }
echo "USER: uiapp.elf entry=$ENTRY ok"

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

# Fase C: compile generated *_img.c -> obj-md (embedded user programs).
for prog in init ucat uls uecho umon ugpio usd ufs face uiapp; do
    # shellcheck disable=SC2086
    clang $CFLAGS -c "$GEN/${prog}_img.c" -o "$OBJMD/${prog}_img.o" >>"$LOG" 2>&1 || \
        { echo "FAIL md/${prog}_img.c (see $LOG)"; exit 1; }
done
echo "MD: *_img.o ok (10 programs)"

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
