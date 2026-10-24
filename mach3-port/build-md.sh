#!/bin/bash
# build-md.sh -- build ARM machine-dependent sources and link mach3.elf.
#
# 1. Runs build-mi.sh (MI objects -> build/obj/, now with ARM MD headers).
# 2. Compiles src/md/arm/*.c|*.s, src/md/arm/tests/*.c and
#    src/ext/*.c (+ ext/littlefs/*.c) -> build/obj-md/.
# 3. Links with mach3.ld -> build/mach3.elf.
set -u

# Portable toolchain (survive VM reset): prefer ~/workspace/toolchain.
if [ -f "$HOME/workspace/toolchain/env.sh" ]; then
    # shellcheck disable=SC1091
    . "$HOME/workspace/toolchain/env.sh"
fi

MACH3_DIR="$(cd "$(dirname "$0")" && pwd)"
# Mach 3.0 MI sources: vendored in-repo (mach3-port/upstream/mach3);
# override with MACH3_SRC=/path/to/mach3 to use an external tree instead.
SRC="${MACH3_SRC:-$MACH3_DIR/upstream/mach3}/kernel"
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

# LLVM binutils: allow override via env, else auto-detect (versioned or not).
if [ -z "${LLVM_OBJCOPY:-}" ]; then
    for c in llvm-objcopy-19 llvm-objcopy-18 llvm-objcopy-17 llvm-objcopy-16 llvm-objcopy; do
        command -v "$c" >/dev/null 2>&1 && { LLVM_OBJCOPY="$c"; break; }
    done
fi
if [ -z "${LLVM_READELF:-}" ]; then
    for c in llvm-readelf-19 llvm-readelf-18 llvm-readelf-17 llvm-readelf-16 llvm-readelf; do
        command -v "$c" >/dev/null 2>&1 && { LLVM_READELF="$c"; break; }
    done
fi
: "${LLVM_OBJCOPY:=llvm-objcopy}" "${LLVM_READELF:=llvm-readelf}"

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
for prog in init ucat uls uecho umon ugpio usd ufs sh utimeset uaudiotest uaudiocap uaudiogain upl330 ulfs ublogtest ufirst utimegate uupdsig ucred ufault ufaultd uyield uhangd uspin uhangc uipcsrv uipccli; do
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
    ENTRY=$("$LLVM_READELF" -h "$BUILD/$prog.elf" | sed -n 's/.*Entry point address: *//p')
    [ "$ENTRY" = "0x100000" ] || \
        { echo "FATAL: $prog entry $ENTRY != 0x100000"; exit 1; }
    "$LLVM_OBJCOPY" -O binary "$BUILD/$prog.elf" "$BUILD/$prog.bin" >>"$LOG" 2>&1 || \
        { echo "FAIL objcopy user/$prog.bin"; exit 1; }
    # Fase D: objcopy -O binary tidak menyertakan .bss (NOBITS) di akhir;
    # pad binary dengan nol hingga akhir .bss agar img_len mencakup
    # footprint memori penuh (loader menghitung npages dari img_len).
    python3 "$USR/pad-bss.py" "$BUILD/$prog.bin" "$BUILD/$prog.elf" \
        >>"$LOG" 2>&1 || \
        { echo "FAIL pad-bss user/$prog.bin"; exit 1; }
    # sh outgrew the 32 KB default (B2-B4 + Boot UX + ONB-1
    # builtins); it gets the uiapp-class cap.
    cap=32768
    [ "$prog" = "sh" ] && cap=65536
    python3 "$USR/embed.py" "$BUILD/$prog.bin" "$GEN/${prog}_img.c" "${prog}_img" $cap >>"$LOG" 2>&1 || \
        { echo "FAIL embed ${prog}_img"; exit 1; }
    echo "USER: $prog.elf entry=$ENTRY ok"
done
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/hello.ld" \
    -o "$BUILD/hello.elf" "$BUILD/hello.o" "$BUILD/ulib.o" >>"$LOG" 2>&1 || \
    { echo "FAIL link user/hello.elf (see $LOG)"; exit 1; }
echo "USER: hello.elf link OK"

# --- 1c. user program face/Qabot (App A1): multi-file (main.c + face.c +
# face_draw.c + qr.c), pola sama: link di 0x100000, objcopy, pad-bss,
# embed. ---
echo "BUILD user program face (Qabot)"
FCFLAGS="$UCFLAGS -I$USR/face -Wno-unused-function"
for f in main face face_draw qr; do
    # shellcheck disable=SC2086
    clang $FCFLAGS -c "$USR/face/$f.c" -o "$BUILD/face_$f.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/face/$f.c (see $LOG)"; exit 1; }
done
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/face.elf" "$BUILD/face_main.o" "$BUILD/face_face.o" \
    "$BUILD/face_face_draw.o" "$BUILD/face_qr.o" "$BUILD/ulib.o" \
    "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/face.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/face.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: face entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/face.elf" "$BUILD/face.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/face.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/face.bin" "$BUILD/face.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/face.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/face.bin" "$GEN/face_img.c" "face_img" 65536 >>"$LOG" 2>&1 || \
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
# NVS persistensi setting (App A4): user/cfg/cfg.c -> cfg.o, link ke uiapp.
# shellcheck disable=SC2086
clang $UCFLAGS2 -c "$USR/cfg/cfg.c" -o "$BUILD/cfg.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/cfg/cfg.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/uiapp.elf" "$BUILD/uiapp_main.o" "$BUILD/uiapp_ui.o" \
    "$BUILD/uiapp_ui_draw.o" "$BUILD/uiapp_scr_menu.o" \
    "$BUILD/uiapp_scr_mon.o" "$BUILD/uiapp_scr_settings.o" \
    "$BUILD/uiapp_scr_power.o" "$BUILD/uiapp_scr_wifi.o" \
    "$BUILD/uiapp_scr_ble.o" "$BUILD/uiapp_scr_llm.o" \
    "$BUILD/uiapp_scr_passkey.o" "$BUILD/uiapp_scr_textedit.o" \
    "$BUILD/uartproto_uartproto.o" "$BUILD/uartproto_mock_comcu.o" \
    "$BUILD/cfg.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/uiapp.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/uiapp.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: uiapp entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/uiapp.elf" "$BUILD/uiapp.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/uiapp.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/uiapp.bin" "$BUILD/uiapp.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/uiapp.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/uiapp.bin" "$GEN/uiapp_img.c" "uiapp_img" 65536 >>"$LOG" 2>&1 || \
    { echo "FAIL embed uiapp_img"; exit 1; }
echo "USER: uiapp.elf entry=$ENTRY ok"

# --- 1d0. user program uvoice (D4): VOICE.START + AUD uplink test. ---
echo "BUILD user program uvoice"
# shellcheck disable=SC2086
clang $UCFLAGS -I"$USR" -c "$USR/uvoice.c" -o "$BUILD/uvoice.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/uvoice.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/uvoice.elf" "$BUILD/uvoice.o" \
    "$BUILD/uartproto_uartproto.o" "$BUILD/uartproto_mock_comcu.o" \
    "$BUILD/cfg.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/uvoice.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/uvoice.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: uvoice entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/uvoice.elf" "$BUILD/uvoice.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/uvoice.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/uvoice.bin" "$BUILD/uvoice.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/uvoice.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/uvoice.bin" "$GEN/uvoice_img.c" "uvoice_img" 32768 >>"$LOG" 2>&1 || \
    { echo "FAIL embed uvoice_img"; exit 1; }
echo "USER: uvoice.elf entry=$ENTRY ok"

# --- 1d. user program ntp (App A4): daemon sinkron jam via NTP. ---
echo "BUILD user program ntp"
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/ntp.c" -o "$BUILD/ntp.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/ntp.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/ntp.elf" "$BUILD/ntp.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/ntp.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/ntp.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: ntp entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/ntp.elf" "$BUILD/ntp.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/ntp.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/ntp.bin" "$BUILD/ntp.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/ntp.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/ntp.bin" "$GEN/ntp_img.c" "ntp_img" 32768 >>"$LOG" 2>&1 || \
    { echo "FAIL embed ntp_img"; exit 1; }
echo "USER: ntp.elf entry=$ENTRY ok"

# --- 1d2. user program utcpcli (Q2a SEMENTARA): uji TCP client. ---
echo "BUILD user program utcpcli"
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/utcpcli.c" -o "$BUILD/utcpcli.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/utcpcli.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/utcpcli.elf" "$BUILD/utcpcli.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/utcpcli.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/utcpcli.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: utcpcli entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/utcpcli.elf" "$BUILD/utcpcli.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/utcpcli.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/utcpcli.bin" "$BUILD/utcpcli.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/utcpcli.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/utcpcli.bin" "$GEN/utcpcli_img.c" "utcpcli_img" 32768 >>"$LOG" 2>&1 || \
    { echo "FAIL embed utcpcli_img"; exit 1; }
echo "USER: utcpcli.elf entry=$ENTRY ok"

# --- 1d3. lib TLS (Q2b): mbedTLS subset + port + API. ---
echo "BUILD lib TLS (mbedTLS subset)"
TLSCFLAGS="--target=arm-none-eabi -march=armv7-a -O1 -fno-builtin -ffreestanding -I$USR/tls/inc -I$USR/tls/mbedtls/include"
for f in "$USR"/tls/mbedtls/src/*.c; do
    base=$(basename "$f" .c)
    # shellcheck disable=SC2086
    clang $TLSCFLAGS -c "$f" -o "$BUILD/tls_$base.o" >>"$LOG" 2>&1 || \
        { echo "FAIL $f (see $LOG)"; exit 1; }
done
TLSOBJS=""
for f in "$USR"/tls/mbedtls/src/*.c; do
    base=$(basename "$f" .c)
    TLSOBJS="$TLSOBJS $BUILD/tls_$base.o"
done
for f in tls_port tls_bio tls_api; do
    # shellcheck disable=SC2086
    clang $UCFLAGS -I"$USR/tls/inc" -I"$USR/tls/mbedtls/include" \
        -c "$USR/tls/$f.c" -o "$BUILD/tls_$f.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/tls/$f.c (see $LOG)"; exit 1; }
    TLSOBJS="$TLSOBJS $BUILD/tls_$f.o"
done
echo "TLS: mbedTLS + port ok"

# --- 1d4. user program utlscli (Q2b SEMENTARA): uji TLS client. ---
echo "BUILD user program utlscli"
python3 "$USR/../tools/pem2c.py" "$USR/tls/testcerts/ca.crt" \
    "$BUILD/ca_pem.c" "qaonic_test_ca_pem" >>"$LOG" 2>&1 || \
    { echo "FAIL pem2c ca.crt (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang $UCFLAGS -c "$BUILD/ca_pem.c" -o "$BUILD/ca_pem.o" >>"$LOG" 2>&1 || \
    { echo "FAIL ca_pem.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang $UCFLAGS -I"$USR/tls/inc" -I"$USR/tls/mbedtls/include" \
    -c "$USR/utlscli.c" -o "$BUILD/utlscli.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/utlscli.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/utlscli.elf" "$BUILD/utlscli.o" "$BUILD/ca_pem.o" \
    $TLSOBJS "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/utlscli.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/utlscli.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: utlscli entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/utlscli.elf" "$BUILD/utlscli.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/utlscli.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/utlscli.bin" "$BUILD/utlscli.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/utlscli.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/utlscli.bin" "$GEN/utlscli_img.c" "utlscli_img" 1048576 >>"$LOG" 2>&1 || \
    { echo "FAIL embed utlscli_img"; exit 1; }
echo "USER: utlscli.elf entry=$ENTRY ok"

# --- 1e. user program qabot (Qabot harness Q1): ReAct loop + mock provider. ---
echo "BUILD user program qabot"
for f in history tools policy provider loop eventlog main; do
    # shellcheck disable=SC2086
    clang $UCFLAGS -c "$USR/qabot/$f.c" -o "$BUILD/qabot_$f.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/qabot/$f.c (see $LOG)"; exit 1; }
done
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/qabot.elf" \
    "$BUILD/qabot_history.o" "$BUILD/qabot_tools.o" \
    "$BUILD/qabot_policy.o" "$BUILD/qabot_provider.o" \
    "$BUILD/qabot_loop.o" "$BUILD/qabot_eventlog.o" \
    "$BUILD/qabot_main.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/qabot.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/qabot.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: qabot entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/qabot.elf" "$BUILD/qabot.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/qabot.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/qabot.bin" "$BUILD/qabot.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/qabot.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/qabot.bin" "$GEN/qabot_img.c" "qabot_img" 32768 >>"$LOG" 2>&1 || \
    { echo "FAIL embed qabot_img"; exit 1; }
echo "USER: qabot.elf entry=$ENTRY ok"

# --- 1f. user program qabotd (Q4): daemon persisten Qabot. ---
# GANTI uqabotr (Q2c selesai): loop interaktif console + face expr +
# provider anonim TCP polos -> proxy AI Horde di host.
echo "BUILD user program qabotd"
for f in history tools policy provider_real loop eventlog json; do
    # shellcheck disable=SC2086
    clang $UCFLAGS -I"$USR/tls/inc" -I"$USR/tls/mbedtls/include" \
        -c "$USR/qabot/$f.c" -o "$BUILD/qabotd_$f.o" >>"$LOG" 2>&1 || \
        { echo "FAIL user/qabot/$f.c (qabotd) (see $LOG)"; exit 1; }
done
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/qabotd.c" -o "$BUILD/qabotd.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/qabotd.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/qabotd.elf" "$BUILD/qabotd.o" \
    "$BUILD/qabotd_history.o" "$BUILD/qabotd_tools.o" \
    "$BUILD/qabotd_policy.o" "$BUILD/qabotd_provider_real.o" \
    "$BUILD/qabotd_loop.o" "$BUILD/qabotd_eventlog.o" \
    "$BUILD/qabotd_json.o" "$BUILD/cfg.o" "$BUILD/ca_pem.o" \
    $TLSOBJS "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/qabotd.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/qabotd.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: qabotd entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/qabotd.elf" "$BUILD/qabotd.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/qabotd.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/qabotd.bin" "$BUILD/qabotd.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/qabotd.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/qabotd.bin" "$GEN/qabotd_img.c" "qabotd_img" 1048576 >>"$LOG" 2>&1 || \
    { echo "FAIL embed qabotd_img"; exit 1; }
echo "USER: qabotd.elf entry=$ENTRY ok"

# --- 1f. user program sh (Q8/Q9 shell): single-file, pola sama. ---
echo "BUILD user program sh"
clang $UCFLAGS -c "$USR/sh.c" -o "$BUILD/sh.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/sh.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/sh.elf" "$BUILD/sh.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/sh.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/sh.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: sh entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/sh.elf" "$BUILD/sh.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/sh.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/sh.bin" "$BUILD/sh.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/sh.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/sh.bin" "$GEN/sh_img.c" "sh_img" 65536 >>"$LOG" 2>&1 || \
    { echo "FAIL embed sh_img"; exit 1; }
echo "USER: sh.elf entry=$ENTRY ok"

# --- 1f. user program utcpkeep (F1 SEMENTARA): TCP keep-alive RX test. ---
echo "BUILD user program utcpkeep"
# shellcheck disable=SC2086
clang $UCFLAGS -c "$USR/utcpkeep.c" -o "$BUILD/utcpkeep.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/utcpkeep.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/utcpkeep.elf" "$BUILD/utcpkeep.o" \
    "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/utcpkeep.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/utcpkeep.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: utcpkeep entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/utcpkeep.elf" "$BUILD/utcpkeep.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/utcpkeep.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/utcpkeep.bin" "$BUILD/utcpkeep.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/utcpkeep.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/utcpkeep.bin" "$GEN/utcpkeep_img.c" "utcpkeep_img" 32768 >>"$LOG" 2>&1 || \
    { echo "FAIL embed utcpkeep_img"; exit 1; }
echo "USER: utcpkeep.elf entry=$ENTRY ok"

# --- 1f2. user program utlskeep (F1 SEMENTARA): TLS keep-alive RX test. ---
echo "BUILD user program utlskeep"
# shellcheck disable=SC2086
clang $UCFLAGS -I"$USR/tls/inc" -I"$USR/tls/mbedtls/include" \
    -c "$USR/utlskeep.c" -o "$BUILD/utlskeep.o" >>"$LOG" 2>&1 || \
    { echo "FAIL user/utlskeep.c (see $LOG)"; exit 1; }
# shellcheck disable=SC2086
clang --target=arm-none-eabi -nostdlib -T "$USR/init.ld" \
    -o "$BUILD/utlskeep.elf" "$BUILD/utlskeep.o" "$BUILD/ca_pem.o" \
    $TLSOBJS "$BUILD/ulib.o" "$BUILD/udiv.o" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL link user/utlskeep.elf (see $LOG)"; exit 1; }
ENTRY=$("$LLVM_READELF" -h "$BUILD/utlskeep.elf" | sed -n 's/.*Entry point address: *//p')
[ "$ENTRY" = "0x100000" ] || \
    { echo "FATAL: utlskeep entry $ENTRY != 0x100000"; exit 1; }
"$LLVM_OBJCOPY" -O binary "$BUILD/utlskeep.elf" "$BUILD/utlskeep.bin" >>"$LOG" 2>&1 || \
    { echo "FAIL objcopy user/utlskeep.bin"; exit 1; }
python3 "$USR/pad-bss.py" "$BUILD/utlskeep.bin" "$BUILD/utlskeep.elf" \
    >>"$LOG" 2>&1 || \
    { echo "FAIL pad-bss user/utlskeep.bin"; exit 1; }
python3 "$USR/embed.py" "$BUILD/utlskeep.bin" "$GEN/utlskeep_img.c" "utlskeep_img" 1048576 >>"$LOG" 2>&1 || \
    { echo "FAIL embed utlskeep_img"; exit 1; }
echo "USER: utlskeep.elf entry=$ENTRY ok"

# --- 2. MD compile ---
mkdir -p "$OBJMD"
: > "$LOG"

CFLAGS="--target=arm-none-eabi -march=armv7-a -DKERNEL -O1 -fno-builtin -fcommon"
OVR="$MACH3_DIR/overrides"
CFLAGS="$CFLAGS -I$OVR -I$GEN -I$INC -I$SRC"
# Restructure: the MD/ext split separated files that used quoted
# includes across the old single arm/ directory (ext/audio.c quotes
# "machine_routines.h" from md/arm). Both source roots are include
# roots so quoted includes resolve as they did pre-split; the
# dependency direction stays ext -> md, never the reverse.
CFLAGS="$CFLAGS -I$MACH3_DIR/src/md/arm -I$MACH3_DIR/src/ext"
CFLAGS="$CFLAGS -I$MACH3_DIR/src/ext/littlefs -I$MACH3_DIR/src/ext/littlefs/shim"
CFLAGS="$CFLAGS -Wno-implicit-function-declaration -Wno-implicit-int"
CFLAGS="$CFLAGS -Wno-int-conversion -Wno-deprecated-non-prototype"
CFLAGS="$CFLAGS -Wno-extra-tokens -include $GEN/md_globals.h"

pass=0; fail=0
for src in "$MACH3_DIR/src/md/arm/"*.c "$MACH3_DIR/src/md/arm/tests/"*.c "$MACH3_DIR/src/ext/"*.c; do
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
# B1: littlefs sources (subdir, own config header).
for src in "$MACH3_DIR/src/ext/littlefs/"*.c; do
    base=$(basename "$src" .c)
    echo "CC md/littlefs/$base.c" >> "$LOG"
    # shellcheck disable=SC2086
    if clang $CFLAGS -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -c "$src" -o "$OBJMD/lfs_$base.o" >> "$LOG" 2>&1; then
        pass=$((pass+1))
    else
        echo "FAIL md/littlefs/$base.c (see $LOG)"
        fail=$((fail+1))
    fi
done
for src in "$MACH3_DIR/src/md/arm/"*.s; do
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
for prog in init ucat uls uecho umon ugpio usd ufs face uiapp ntp utcpcli utlscli qabot qabotd sh utimeset utcpkeep utlskeep uaudiotest uaudiocap uaudiogain uvoice upl330 ulfs ublogtest ufirst utimegate uupdsig ucred ufault ufaultd uyield uhangd uspin uhangc uipcsrv uipccli; do
    # shellcheck disable=SC2086
    clang $CFLAGS -c "$GEN/${prog}_img.c" -o "$OBJMD/${prog}_img.o" >>"$LOG" 2>&1 || \
        { echo "FAIL md/${prog}_img.c (see $LOG)"; exit 1; }
done
echo "MD: *_img.o ok (16 programs)"

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
exit 0
