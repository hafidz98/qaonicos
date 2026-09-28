#!/bin/bash
# build-mi.sh -- compile ALL Mach 3 MI sources with clang (no link).
#
# MI = machine-independent kernel: kern, ipc, vm, device, ddb
#      (+ kern/profile.c, kern/server_loop.c: in-tree but absent from
#      conf/files upstream; we try them anyway)
# MD stand-in for <machine/*.h> / <mach/machine/*.h>: the mips port
# headers (proven by the M1 probe).  Real ARM MD headers are M3's job.
#
# Generated headers (config + MIG stubs) go to build/gen/.
# Object files go to build/obj/<dir>/.
# A per-file log goes to build/compile.log; summary printed at end.
set -u

MACH3_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${MACH3_SRC:-$HOME/workspace/mach3-src}/kernel"
BUILD="$MACH3_DIR/build"
GEN="$BUILD/gen"
INC="$BUILD/inc"
OBJ="$BUILD/obj"
LOG="$BUILD/compile.log"

# --- tools (reinstall-tolerant; VM swaps wipe /usr/bin) ---
if ! command -v clang >/dev/null 2>&1; then
    echo "clang missing, installing (standing authority 2026-09-27)..." >&2
    sudo apt-get install -y clang >/dev/null 2>&1 || \
        sudo apt-get install -y clang --no-install-recommends >&2
fi
command -v clang >/dev/null 2>&1 || { echo "FATAL: clang unavailable"; exit 1; }

# --- directory layout ---
mkdir -p "$GEN" "$OBJ" "$INC" "$INC/mach"
# <machine/*.h>  -> ARM MD headers (M3: real, was mips stand-in in M2)
ln -sfn "$MACH3_DIR/kernel/arm" "$INC/machine"
# <mach/machine/*.h> -> ARM mach/machine headers
ln -sfn "$MACH3_DIR/kernel/mach/arm" "$INC/mach/machine"

# --- generated headers ---
python3 "$MACH3_DIR/tools/gen_config.py" "$GEN"
python3 "$MACH3_DIR/tools/gen_mig_stubs.py" "$GEN"

# --- compiler flags ---
# -DKERNEL is MANDATORY (mach/vm_param.h deliberately #errors without it).
# -Wno-implicit-function-declaration: 1990s C predates C99 prototypes.
CFLAGS="--target=arm-none-eabi -DKERNEL -O1 -fno-builtin -fcommon"
# M3: mi-overrides/ first so converted headers (e.g. ddb/db_output.h)
# shadow pristine MI headers.
OVR="$MACH3_DIR/mi-overrides"
CFLAGS="$CFLAGS -I$OVR -I$GEN -I$INC -I$SRC"
CFLAGS="$CFLAGS -Wno-implicit-function-declaration"
# clang >= 16 promotes implicit-int to a hard error; 1990s C uses it.
CFLAGS="$CFLAGS -Wno-implicit-int"
# Pointer<->integer conversions are pervasive and intentional in 1993
# code (e.g. kernel passes ipc_port_t to MIG stubs taking mach_port_t);
# gcc 2.x accepted them silently.
CFLAGS="$CFLAGS -Wno-int-conversion"
# MD-provided globals referenced by MI (e.g. boothowto); see md_globals.h.
CFLAGS="$CFLAGS -include $GEN/md_globals.h"
CFLAGS="$CFLAGS -Wno-deprecated-non-prototype -Wno-extra-tokens"

# --- MI source list (conf/files + optionals, see tools/mi_sources.py) ---
SRCS="$(python3 "$MACH3_DIR/tools/mi_sources.py" "$SRC")"

# server_loop.c is a stray that expects -DSERVER_NAME from the build
# (it is a generic server-loop template, not in conf/files upstream).
SERVER_DEFS=""
if echo "$SRCS" | grep -q "^kern/server_loop.c$"; then
    SERVER_DEFS='-DSERVER_NAME="\"mach_kernel\"" -DSERVER_DISPATCH=mach_server_routine'
fi

: > "$LOG"
pass=0; fail=0; failed=""
for rel in $SRCS; do
    src="$SRC/$rel"
    # M3: MI override copies (varargs->stdarg.h conversions) win over pristine source.
    if [ -f "$OVR/$rel" ]; then src="$OVR/$rel"; fi
    obj="$OBJ/${rel%.c}.o"
    mkdir -p "$(dirname "$obj")"
    extra=""
    case "$rel" in
        kern/server_loop.c) extra="$SERVER_DEFS" ;;
    esac
    if clang $CFLAGS $extra -c "$src" -o "$obj" >>"$LOG" 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        failed="$failed $rel"
        echo "FAIL: $rel" | tee -a "$LOG" >&2
    fi
done

echo "----------------------------------------"
echo "MI compile: $pass passed, $fail failed"
if [ -n "$failed" ]; then
    echo "failed files:$failed"
fi
echo "full log: $LOG"
[ "$fail" -eq 0 ]
