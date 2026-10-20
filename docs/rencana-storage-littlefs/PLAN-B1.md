# B1 Plan: Storage — littlefs on QaonicOS

Date: 2026-10-20. Phase B1 of the out-of-the-box roadmap
(`docs/riset-out-of-box/RINGKASAN.md`). Goal: a power-loss-safe
filesystem for user data, replacing FAT32's corruption risk.

## 1. Why littlefs
- **Power-loss safe by design** (copy-on-write metadata pairs):
  a power cut mid-write leaves either the old or the new state,
  never corruption. FAT32 has no such guarantee.
- **Wear leveling** built in (needed for NAND later).
- **Bounded RAM**: memory use does not grow with filesystem size —
  fits the 64MB budget.
- **Small**: ~15KB code, single-file-pair (`lfs.c`/`lfs.h`),
  permissive license (MIT-style).

## 2. Architecture
```
apps / shell
  -> existing file syscalls (sys_open/read/write/close) — unchanged
kernel VFS-ish layer
  -> fat32.c   (SD / QEMU dev disk, stays)
  -> lfs glue  (NEW: kernel/kernel/arm/lfs_port.c)
littlefs core (lfs.c)
  -> block device callbacks (read/prog/erase/sync)
     -> QEMU: virtio-blk (test now)
     -> hardware: SPI NAND driver (B1-HW, needs board)
```

Design decision: littlefs mounts on a dedicated partition/device,
FAT32 stays for the SD/removable side. User data (config, logs,
recordings) lives on littlefs; the shell gains `lfs` test commands
first, then user-facing paths are migrated.

## 3. Tasks
- [ ] B1.1 Vendor littlefs source into `kernel/kernel/arm/littlefs/`
      (pin a release tag; record version + license).
- [ ] B1.2 Block device glue over virtio-blk (512B sectors):
      read/prog = sector write, erase = write 0xFF-filled block
      (emulated on blk; real erase only matters on NAND).
      Config: block_size 4096, block_count from device size.
- [ ] B1.3 Kernel mount/format API + syscall(s) for format/mount/
      stat; test program `user/ulfs.c`:
      format -> write file -> read back -> verify -> reboot-less
      remount -> verify persistence.
- [ ] B1.4 Power-cut simulation test: write pattern, "crash"
      (skip sync / hard QEMU kill mid-write), remount, verify
      filesystem mounts and old-or-new state is consistent.
      Marker: `ULFS: PASS`.
- [ ] B1.5 Shell integration: `ls`/`cat` can target the lfs mount
      (e.g. `/data/...`) alongside `/sd`.
- [ ] B1.6 Dev log + docs update (`docs/UTANG-TEKNIS.md` if new
      debt appears).

## 4. Out of scope (needs board)
- SPI NAND controller driver (SFC) + real erase/ECC/bad blocks.
- Wear-level validation on real NAND.
- Migrating the NAND rootfs partition to littlefs/UBIFS.

## 5. Acceptance criteria (QEMU)
- `ULFS: PASS` (format/write/read/remount/verify).
- Power-cut test: filesystem mounts after simulated crash, data is
  old-or-new, never garbage.
- Boot still reaches `qaon>`, all previous tests still pass.
