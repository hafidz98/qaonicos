# Roadmap QaonicOS — Fase 13–18 (2026-09-28)

Target user: **Shell, filesystem, GPIO, SD Card, Boot Menu, TUI**.
Aturan: **shell tetap paling akhir**.

## Urutan fase

| Fase | Nama | Isi |
|------|------|-----|
| 13 | Boot menu | Menu pilih boot via UART sebelum kernel jalan: boot QaonicOS / boot + self-test / (nanti: shell). Timeout auto-boot. |
| 14 | GPIO | Driver GPIO: register asli RV1103 (Rockchip GPIO, cek `hw-addrs.md`/DTS) + mock GPIO virtual di QEMU `-M virt` (MMIO palsu, supaya jalur driver→syscall→userspace bisa dites). Syscall `SYS_GPIO_SET/GET`, program userspace `ugpio`. |
| 15 | SD Card | Abstraksi block device (`blkdev`). Backend QEMU = virtio-blk kedua (`sd128.img`, terpisah dari `pico128.img` milik Fase 12d). Backend RV1103 = driver dw_mmc/SDMMC (tulis lawan alamat SDK, compile-check saja di QEMU). |
| 16 | Filesystem | FAT32 di atas `blkdev`, mount di `/sd`. Image FAT32 dibuat di host (script python, tanpa tools eksternal). Syscall file diperluas (mkdir/readdir). |
| 17 | TUI | Library text-UI di atas console UART: panel, status bar, input. Program `umon`: system monitor TUI pakai statistik real Fase 12d. |
| 18 | Shell | Interactive shell, builtin: `ls cat echo gpio mount umon help`. **Paling akhir.** |

## Catatan pengujian
- Semua fase diuji di QEMU `-M virt -cpu cortex-a7` (board `BOARD_VIRT`), kriteria: N/N run stabil seperti fase sebelumnya.
- Driver hardware asli RV1103 (GPIO register, dw_mmc) ditulis lawan alamat SDK/DTS dan wajib lolos compile clang `--target=arm-none-eabi`; runtime test hanya untuk backend QEMU.
- Commit per fase. Toolchain: clang (gcc sudah diganti total).
