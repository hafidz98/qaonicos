# Mach3 Port M3 Report: ARM Machine-Dependent Code + First Boot

**Date:** 2026-09-28  
**Status:** MD complete, link OK, boot partial (banner + VM init, panic in kmem_suballoc - fix ready)

## Accomplishments

### 1. ARM Machine-Dependent Headers (31 files)
- `mach3/kernel/mach/arm/` (8 headers): vm_types.h, vm_param.h, boolean.h,
  kern_return.h, exception.h, machine_types.defs, syscall_sw.h, thread_status.h
- `mach3/kernel/arm/` (23 headers): machspl.h, cpu.h, cpu_number.h, lock.h,
  ast.h, ast_types.h, timer.h, time_stamp.h, vm_tuning.h, sched_param.h, xpr.h,
  mach_param.h (+HZ=100), mp/mp.h, thread.h, setjmp.h, pmap.h, pte.h,
  db_machdep.h, db_trace.h, kttd_machdep.h, machine_routines.h, asm.h,
  asm_linkage.h, regdef.h, vm_param.h

### 2. ARM Machine-Dependent Sources (16 files)
- `uart.c`: PL011 UART driver (0x09000000)
- `gic.c`: GIC-400 interrupt controller (0x08000000/0x08010000)
- `locore.s`: Vector table, MMU bootstrap, trap frame setup
- `arm_init.c`: `_start` → `mips_init` → `pmap_bootstrap` → `machine_startup`
- `pmap.c`: Identity-mapped pmap (ARMv7 short descriptors)
- `trap.c`: Exception dispatch (IRQ, data abort, prefetch abort, SVC, etc.)
- `pcb.c`: PCB management, stack_alloc/stack_free, Switch_context
- `context.s`: `__Switch_context`, `load_context`, `call_continuation`
- `clock.c`: ARM virtual timer (100Hz), `clock_interrupt`
- `copy.s`: `copyin`, `copyout`, `copyinmsg`, `copyoutmsg`
- `fpu.c`: VFP enable
- `machdep.c`: `machine_init`, `halt_cpu`, `Debugger`, byte order
- `aeabi.c`: ARM EABI helpers (`__aeabi_uidiv`, etc.)
- `mig_stubs.c`: MIG client stubs (panic if called - no pager in M3)
- `db_stubs.c`: ddb debugger stubs (panic on entry)
- `devices.c`: Empty device tables

### 3. Build System
- `mach3.ld`: Linker script (kernel at 0x40000000, 64MB RAM)
- `build-md.sh`: Compiles MD sources, links with MI objects
- `build-mi.sh`: Updated with `-fcommon`, `-DSERVER_DISPATCH`

### 4. Link Result
- **LINK OK:** `build/mach3.elf` (461KB, ARM 32-bit, statically linked)
- All 94 MI files + 16 MD files compile and link
- Zero MI source patches (all adaptations via headers/flags/overrides)

### 5. Boot Test (QEMU `-M virt -cpu cortex-a7 -m 64`)
```
Mach 3.0 (ARMv7 port, QEMU virt)
vm_page_bootstrap: 11923 free pages
kmem_suballoc: parent=43003294 min=40000000 max=44000000 size=c00000
kmem_suballoc: vm_map_enter kr=0 addr=4310a000  (zone_map: SUCCESS)
kmem_suballoc: parent=... size=800000
kmem_suballoc: vm_map_enter kr=3 addr=40000000  (ipc_map: KERN_NO_SPACE)
panic: kmem_suballoc kr=3
```

**Progress:** Banner prints, VM system initializes (11923 free pages),
zone allocator succeeds. Fails on second submap (ipc_kernel_map) due to
virtual space exhaustion.

## Known Issue: kmem_suballoc KERN_NO_SPACE

**Root cause:** The MI `vm_page_bootstrap` uses `virtual_space_start`
(0x4310a000, the steal-memory pointer) for `kmem_init`, instead of
`avail_start` (0x40063000, end of kernel image). This reserves 17MB
unnecessarily, leaving insufficient space for submaps.

**Fix (ready in source, not yet compiled):** MD `pmap_startup()` override
in `kernel/arm/pmap.c` sets `*startp = avail_start`. Requires
`--allow-multiple-definition` linker flag (already added to build-md.sh).

**Blocker:** Clang toolchain unavailable (VM reset wiped /usr/bin).
apt-get update too slow (179s for headers). Rebuild pending.

## Design Decisions
- Identity mapping: VA = PA (simplifies pmap)
- Kernel linked at 0x40000000, RAM 0x40000000-0x44000000 (QEMU -m 64)
- PL011 UART at 0x09000000 (QEMU virt default)
- GIC-400 at 0x08000000/0x08010000 (QEMU virt)
- ARM virtual timer PPI 30, 100Hz (HZ=100)
- `pmap_enter`: no-op for identity range, panic outside (M3 limitation)
- MIG stubs panic if called (no pager/task servers in M3 boot)
- ddb compiled in but panics on entry (full ddb is future work)

## Limitations (M3)
- No user tasks (thread_exception_return panics)
- No real VM mappings (identity only)
- No device drivers (empty device tables)
- No IPC servers (MIG stubs are panic-stubs)
- `pmap_enter` panics for non-identity addresses

## Next Steps (M4)
1. Rebuild with `pmap_startup` fix → boot to `setup_main` completion
2. Implement real `pmap_enter` with 4KB pages (or larger virtual space)
3. Bring up IPC and create first task
4. Device driver for virtio-blk (storage)
5. User mode and syscall interface

## Files
- MD headers: `mach3/kernel/arm/`, `mach3/kernel/mach/arm/`
- MD sources: `mach3/kernel/arm/*.c`, `mach3/kernel/arm/*.s`
- Build: `mach3/build-md.sh`, `mach3/build-mi.sh`, `mach3/mach3.ld`
- Output: `mach3/build/mach3.elf`
- MI overrides: `mach3/mi-overrides/` (5 varargs conversions + vm_kern debug)

## Debug boot 2026-09-28 (post-commit c376be7)

Tiga bug ditemukan dan difix saat boot test:

1. **MD `pmap_startup` tidak mengisi free list** — override MD me-replace
   versi generik MI di `vm_resident.c` yang mengisi free list via
   `pmap_next_page`/`vm_page_init`/`vm_page_release`. Fix: versi MD kini
   mereplikasi logika populasi MI. (Gejala: `vm_page_bootstrap: 0 free
   pages` -> `panic: vm_page_grab`)

2. **Dua kursor steal-memory terpisah (KRITIS)** — clang meng-inline MI
   generik `pmap_steal_memory` ke dalam `vm_page_bootstrap` (satu TU),
   memakai kursor global `virtual_space_start`, sementara panggilan dari
   TU lain memakai override MD dengan kursor `virt_steal_next`. Akibatnya
   array `pages` (1.3MB) menimpa `zdata`/kentry/buckets -> free list
   terkorup -> `panic: vm_page_grab` yang misterius. Fix: `#define
   MACHINE_PAGES` di `kernel/arm/pmap.h` (seperti port mips/alpha) agar
   versi generik MI tidak dikompilasi sama sekali; satu kursor.

3. **Macro GIC salah** — `GICD_IPRIORITYR(n)` = `(0x400u + (n))`, kurang
   `<< 2`, menyebabkan store word tidak-aligned ke MMIO GIC ->
   alignment fault (`dfar=0x08000401`). Fix: `(0x400u + ((n) << 2))`.

Hasil: kernel boot stabil sampai idle loop (`setup_main` ->
`cpu_launch_first_thread` -> `start_kernel_threads` -> idle).
`vm_page_bootstrap: 12047 free pages`, 3x `kmem_suballoc` sukses,
tidak ada panic/fault dalam 15 detik run. Idle loop Mach 3.0 adalah
spin `while(TRUE)` (tanpa WFI), jadi CPU host ~97% adalah normal.
