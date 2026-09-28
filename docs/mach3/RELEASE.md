# Mach3 ARMv7 Port — Production Release v1.0

**Tag:** `mach3-port-v1.0`
**Date:** 2026-09-28
**Target:** QEMU `virt` (Cortex-A7), RV1103 (future)

CMU Mach 3.0 microkernel ported to ARMv7. Zero patches to original
Mach 3.0 source (`~/workspace/mach3-src` untouched).

## What's Included

### M1: Inventory & Scaffolding
- MI source inventory (~250 files, ~101k lines).
- 44 MD ARM stub files (headers + sources).
- Clang 18 cross-compile probe.

### M2: MI Compilation
- 94/94 MI files compile with clang 18, 0 errors.
- Build system: `build-mi.sh`, `gen_config.py`, `gen_mig_stubs.py`.

### M3: MD Implementation & Boot
- 31 ARM headers + 16 sources (UART, GIC, pmap, trap, pcb, context, clock).
- Boots in QEMU: `Mach 3.0 (ARMv7 port, QEMU virt)` banner.
- `pmap_startup` fix for kmem allocation.

### M4: Core Services
- **pmap**: 4KB real mappings, TTBR0 switching.
- **IPC**: Kernel port alloc/dealloc, message send/receive.
- **Timer**: 100Hz via ARM virtual timer (PPI 27).
- **virtio-blk**: DMA-coherent block driver.
- **User mode**: SYS_WRITE/SYS_EXIT via SVC, fault isolation.

### M5: Hardening
- 30x boot stability (29/30 PASS).
- IPC stress (400 ports, 200 msgs, no leak).
- pmap stress (1930 ops, no stale TLB).

### M6: Scheduler (this release)
- **Context switch fix**: `PCB_KSS` corrected 92→80
  (`struct arm_saved_state` is 20 words, not 23).
- **Cooperative multitasking**: 2 threads interleave
  (`A0 B0 A1 B1 ... A7 B7`), `sched_selftest: PASS`.
- **thread_exception_return**: Minimal ARM implementation
  (AST hook point for future preemption).

## Test Results (QEMU)

```
pmap_selftest: PASS (4KB L2 mapping, TTBR0 switch, R/W)
pmap_stress: PASS (1930 ops, no stale mappings)
ipc_selftest: PASS (kernel port alloc/dealloc)
ipc_stress: PASS (400 ports, 200 msgs FIFO, no leak)
task_selftest: PASS (task_create + thread_create + run)
user_selftest: PASS (user mode + syscall + fault isolation)
sched_selftest: PASS (cooperative interleave A/B)
```

## Known Limitations

1. **Preemption**: Cooperative switching only. Timer-driven AST
   preemption requires per-thread trap frames (trap handler currently
   uses global SVC stack). Future work.
2. **User memory isolation**: Basic (priv-only kernel sections, fault
   kills thread). Full VM isolation via pmap per-task is future work.
3. **SMP**: Uniprocessor only.
4. **Drivers**: UART, GIC, timer, virtio-blk only. No network, no display.

## Building

```bash
source ~/workspace/toolchain/env.sh
cd mach3
./build-mi.sh   # MI sources
./build-md.sh   # MD sources + link
```

Output: `build/mach3.elf` (ARM 32-bit, ~461KB).

## Running (QEMU)

```bash
source ~/workspace/toolchain/env.sh
qemu-system-arm -M virt -cpu cortex-a7 -m 64 -nographic \
    -kernel mach3/build/mach3.elf
```

## Next: RV1103 Hardware

Pivot to real Luckfox Pico Mini hardware:
- Replace QEMU virt drivers with RV1103 (UART2 @ 0xff4c0000, etc.).
- Reference: `~/workspace/luckfox-pico-mini-b/` (datasheet, schematics).
