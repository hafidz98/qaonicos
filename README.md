# QaonicOS

Sistem operasi untuk **Luckfox Pico Mini** (Rockchip RV1103, Cortex-A7, 64MB RAM),
dikembangkan dan diuji di QEMU `-M virt -cpu cortex-a7` sebelum dibawa ke hardware asli.

## Struktur

```
qaonic_os/
├── kernel/          # Kernel QaonicOS: kernel_main.c + build.sh + src/ (modul: trap, pmap,
│                    #   sched, ipc, vm, net stack, driver virtio-blk/net, FAT32, GPIO, ...)
├── user/            # Program userspace: init, ucat, uls, uecho, ugpio, usd, ufs, umon + ulib
├── tools/           # mkfat32.py — generator image FAT32 (Python murni)
├── scripts/         # run-qemu.sh — boot QEMU dengan spek Pico Mini (-m 64, virtio-blk, net)
├── mach3/           # Track porting kernel Mach 3 ASLI (CMU) ke ARMv7 — RILIS v1.0 ✅
├── hw/dts/          # Device tree RV1103 (referensi alamat hardware)
├── docs/            # Dokumentasi: analisis porting, roadmap, hw-addrs, laporan M1–M6 mach3, aset gambar
├── disk-images/     # pico128.img (storage) + sd128.img (SD) — dibuat otomatis, tidak di-commit
└── archive/         # Eksperimen/tes lama (qemu-test, test-ctx, test-fpu, test-pmap, test-trap)
```

## Cara build & run (QEMU)

```sh
./kernel/build.sh        # build kernel -> kernel/mach-kernel.elf
./scripts/run-qemu.sh    # boot (Ctrl-A X untuk keluar)
```

## Direktori terkait (di luar repo ini)

- `~/workspace/mach3-src/` — source CMU Mach 3.0 asli (18MB, **tidak di-patch**)
- `~/workspace/toolchain/` — toolchain portable (clang-18 + lld + qemu-system-arm 8.2.2)
- `~/workspace/luckfox-pico-mini-b/` — referensi hardware: datasheet RV1103 V1.4 + schematic

## Status

- **QaonicOS mainline**: user mode + syscall, ramfs, init + utilitas, boot menu, GPIO,
  SD card, FAT32, TUI system monitor (`umon`), web server system monitor (HTTP di port 80).
- **mach3 track**: port Mach 3.0 asli ke ARMv7 — production release `mach3-port-v1.0`
  (pmap 4KB, IPC, virtio-blk, user mode + syscall, scheduler). Lihat `docs/mach3/RELEASE.md`.
- **Berikutnya**: bring-up hardware RV1103 asli (blocker: DRAM init tanpa TRM publik).
