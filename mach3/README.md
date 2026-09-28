# QaonicOS "mach3" track — porting kernel Mach 3 ASLI ke ARMv7

Track ini mem-port **CMU Mach 3.0 asli** (bukan tiruan) ke ARMv7
(Cortex-A7, target: QEMU `-M virt` dulu, Luckfox Pico Mini / RV1103 kemudian).

## Di mana source Mach 3 aslinya?

**TIDAK di-copy ke repo ini** (18MB). Lokasi persisten:

```
~/workspace/mach3-src/        # git clone https://github.com/Prajna/mach (CMU Mach 3.0)
```

Struktur source asli:

```
mach3-src/
  kernel/kern/     # MI: task, thread, scheduler, lock, clock  (~31k baris)
  kernel/ipc/      # MI: IPC, ports, MIG runtime               (~23k baris)
  kernel/vm/       # MI: virtual memory, vm_object, pmap API    (~21k baris)
  kernel/mach/     # MI: header publik + .defs (MIG)            (~5.7k baris)
  kernel/device/   # MI: device interface                      (~11k baris)
  kernel/ddb/      # MI: kernel debugger                       (~9.7k baris)
  kernel/mips/     # MD: port MIPS (PMAX) — REFERENSI pola port
  kernel/alpha/    # MD: port Alpha
  kernel/i386/     # MD: port i386
  kernel/mach/{mips,alpha,i386}/  # MD: mach/machine/* per arsitektur
```

Repo ini hanya berisi **lapisan machine-dependent ARM** yang kita tulis:

```
mach3/
  kernel/arm/          # <-- dikompilasi sebagai <machine/*.h> (pengganti kernel/mips/)
  kernel/mach/arm/     # <-- dikompilasi sebagai <mach/machine/*.h>
  README.md            # file ini
  M1-REPORT.md         # laporan tahap M1
  Makefile             # (placeholder, build penuh di M2)
```

## Cara pakai (pola build)

MI di-include dengan path logis; build harus memetakan:

| Include di kode MI | Dipetakan ke |
|---|---|
| `<machine/foo.h>` | `mach3/kernel/arm/foo.h` |
| `<mach/machine/foo.h>` | `mach3/kernel/mach/arm/foo.h` |
| `<kern/...>`, `<ipc/...>`, `<vm/...>`, `<mach/...>`, `<device/...>` | `~/workspace/mach3-src/kernel/...` |

Flag wajib: `-DKERNEL` (tanpa ini, `mach/vm_param.h` sengaja gagal
compile: "YOU HAVE MADE A MISTAKE BY INCLUDING THIS FILE").
Flag bantu: `-Wno-implicit-function-declaration` (kode C 1990-an).

Header config yang di-generate build system asli (`cpus.h`,
`mach_*.h`, `platforms.h`, `simple_clock.h`, `stat_time.h`, …)
harus disediakan — via stub atau via porting `conf/` config tool (M2).

Header MIG (`mach/memory_object_user.h`, `mach/memory_object_default.h`, …)
di-generate dari `.defs` oleh MIG — butuh MIG yang jalan (M2).

## Status

- **M1** (selesai): inventarisasi + kerangka MD ARM + probe compile. Lihat M1-REPORT.md.
- **M2** (selesai): seluruh MI (94 file: kern/ipc/vm/device/ddb) ter-compile
  dengan clang 18 `--target=arm-none-eabi`, 0 error, 0 patch source MI.
  Build: `mach3/build-mi.sh` (generators: `tools/gen_config.py`,
  `tools/gen_mig_stubs.py`, `tools/mi_sources.py`). Header
  `<machine/*.h>` masih stand-in mips; stub MIG compile-only.
  Lihat M2-REPORT.md + MI-PATCHES.md.
- **M3** (rencana): header MD ARM nyata + implementasi MD (adaptasi
  `rv1103-bringup/`) + MIG asli/manual + link + boot di QEMU
  `-M virt -cpu cortex-a7`.
