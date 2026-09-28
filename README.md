# QaonicOS

Sistem operasi untuk **Luckfox Pico Mini** (Rockchip RV1103, Cortex-A7, 64MB RAM),
dikembangkan dan diuji di QEMU `-M virt -cpu cortex-a7` sebelum dibawa ke hardware asli.

## Struktur

```
qaonic_os/
├── kernel/          # KERNEL: Mach 3.0 asli (CMU), port ARMv7 (rilis v1.0).
│                    #   kernel/arm/* = lapisan machine-dependent ARM,
│                    #   build-md.sh/build-mi.sh = build, docs/ = laporan M1-M6 + RELEASE
├── user/            # Program userspace (Fase C: porting ke syscall Mach 3)
├── tools/           # mkfat32.py — generator image FAT32 (Python murni)
├── scripts/         # run-qemu.sh — boot kernel Mach 3 di QEMU
├── hw/dts/          # Device tree RV1103 (referensi alamat hardware)
├── docs/            # Dokumentasi: analisis porting, roadmap, hw-addrs, aset gambar
├── disk-images/     # Image QEMU (dibuat otomatis, tidak di-commit)
└── archive/         # kernel-scratch/ = kernel from-scratch Fase 1-17 (ARSIP, referensi
                     #   driver net/FAT32/GPIO + net stack untuk Fase D) + eksperimen lama
```

## Cara build & run (QEMU)

```sh
cd kernel && ./build-md.sh   # build -> kernel/build/mach3.elf (MI 94/94, MD 22/22)
./scripts/run-qemu.sh        # boot (Ctrl-A X untuk keluar)
```

Self-test saat boot: pmap, ipc, blk, task, user, sched — semua PASS (rilis v1.0).

## Direktori terkait (di luar repo ini)

- `~/workspace/mach3-src/` — source CMU Mach 3.0 asli (18MB, **tidak di-patch**)
- `~/workspace/toolchain/` — toolchain portable (clang-18 + lld + qemu-system-arm 8.2.2)
- `~/workspace/luckfox-pico-mini-b/` — referensi hardware: datasheet RV1103 V1.4 + schematic

## Status

- **Kernel**: Mach 3.0 asli (CMU), port ARMv7 — rilis `v1.0`
  (pmap 4KB, IPC, virtio-blk, user mode + syscall minimal, scheduler kooperatif).
- **Fase A** (selesai): kernel Mach 3 jadi kernel QaonicOS; kernel from-scratch
  Fase 1-17 diarsipkan di `archive/kernel-scratch/` sebagai referensi.
- **Berikutnya**: Fase B (syscall + task/thread userland), Fase C (porting userland/ulib),
  Fase D (driver net/GPIO/SD/FAT32 + net stack + HTTP di atas Mach 3, lalu hardware RV1103).

## Proyek terkait

- `~/workspace/mach3-port/` — track riset terpisah: porting kernel Mach 3.0 asli (CMU)
  ke ARMv7, rilis `v1.0`. Hasil risetnya dipakai sebagai referensi arsitektur QaonicOS.
