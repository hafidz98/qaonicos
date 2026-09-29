# Riset: Porting Mach Kernel ke Luckfox Pico Mini

Tanggal riset: 26 September 2026
Status: riset selesai — porting dinilai layak secara teknis, tapi ini proyek besar (bukan kerjaan sekali duduk).

---

## Kesimpulan

**Bisa, tapi dikerjakan dari nol.** Tidak ada port Mach klasik ke ARM 32-bit yang terbuka dan bisa dipakai ulang — semua kode machine-dependent untuk ARM harus ditulis baru. Kabar baiknya: desain Mach memang memisahkan lapisan machine-dependent yang kecil, dan board-nya cocok (RAM 64MB itu lega untuk Mach; Mach 3.0 dulu jalan di mesin 8MB).

- **Estimasi realistis: 3–6 bulan** untuk insinyur low-level berpengalaman sampai ke kernel multi-thread dengan IPC di console serial.
- **Risiko terbesar bukan silikon, tapi dokumentasi:** tidak ada TRM publik untuk RV1103, jadi alamat GIC, timer, dan DRAM base harus digali dari device tree SDK.
- **Referensi terdekat:** `darwin-on-arm` (XNU di ARMv7 non-Apple, pernah boot di Nokia N900) untuk struktur bring-up ARM 32-bit.
- **Board yang disarankan: Pico Mini A** (slot microSD — iterasi flashing dalam hitungan detik).

---

## Bagian A — Mach Kernel

### A1. Arsitektur
Mach dibangun di atas **lima abstraksi**: task, thread, port, message, memory object — dengan praktis satu trap system call (`mach_msg`). Kernel terbagi jadi:
- **Machine-independent** (`kern/`, `vm/`, `ipc/`, `device/`)
- **Machine-dependent** (`i386/`, `ppc/`, …) — antarmuka MMU-nya kecil dan tetap (satu referensi mendokumentasikan **~16 rutin pmap** sebagai batas machine-dependent).

Entry kernel: `kern/startup.c` → `setup_main()`. Pemisahan MD/MI yang bersih inilah yang bikin porting tractable: yang ditulis ulang hanya lapisan MD, bukan kernelnya.

### A2. Rilis open-source
- **GNU Mach 1.8** (18 Des 2016), lisensi GPL, sumber di `git.savannah.gnu.org/cgit/hurd/gnumach.git`. **Di rilis resmi hanya IA-32.** Turunan dari Utah Mach 4 ← CMU Mach 3.0.
- **CMU Mach 3.0**: sumbernya bertahan di arsip Utah Flux, Bitsavers, dan mirror GitHub `Prajna/mach` (build via ODE atau plain make).

### A3. Arsitektur yang pernah didukung — dan apakah ada port ARM?
Port historis yang terdokumentasi: VAX, IBM RT/PC, Sun 68030, Encore Multimax, Sequent Balance, MIPS, m88k, Alpha, PA-RISC, PowerPC (MkLinux). **Tidak ditemukan port Mach 3.0/4 klasik ke ARM 32-bit** meski sudah dicari berulang. Entri "ARM32" di infobox Wikipedia tidak terverifikasi — kemungkinan besar tertukar dengan lineage XNU/iOS. Yang benar-benar ada:
1. **GNU Mach AArch64** (Sergey Bugaev, branch `wip-aarch64`, 2024): boot di `qemu-system-aarch64 -M virt`, 11 test kernel lolos. **64-bit saja — tidak bisa dipakai di Cortex-A7 32-bit.**
2. **Port Darwin internal Apple ke ARMv5te** (2010, "ARMing the Snow Leopard", Marvell Sheeva) — tidak terbuka.
3. **darwin-on-arm** (`github.com/darwin-on-arm/xnu`): XNU di ARMv7/ARMv6-A untuk hardware non-Apple (OMAP3530/335X, ARMPBA8…), pernah boot di Nokia N900. **Dormant sejak ~2017, tapi ini referensi terdekat untuk bring-up Mach-family di ARM 32-bit.**

### A4. Yang harus diimplementasikan untuk port baru
1. **Startup awal** (assembly `locore`): entry dari loader, setup stack, lompat ke `setup_main()` di C.
2. **pmap**: manajemen page table di balik ~16 rutin antarmuka MD. Di ARMv7-A bisa mulai dengan mapping section 1MB — jauh lebih sederhana dari fine pages.
3. **Exception vectors**: handler undefined/SVC/prefetch-abort/data-abort/IRQ.
4. **Context switch**: save/restore state thread.
5. **Interrupt controller + clock**: driver GIC dan timer untuk scheduler tick (butuh base address SoC — inilah gap RV1103).
6. **Console**: driver UART polled untuk `printf` awal.
7. **Toolchain + loader**: cross toolchain (SDK Luckfox sudah menyediakan `arm-rockchip830-linux-uclibcgnueabihf`); kernel dimuat via U-Boot.

### A5. Hubungan Mach–XNU
XNU = **Mach (`osfmk/`) + lapisan BSD + I/O Kit + `pexpert/`**, lisensi **APSL 2.0** (dideskripsikan sebagai "rather restrictive"). Kode ARM-nya Apple-SoC-specific. Kesimpulan praktis: pakai **darwin-on-arm** sebagai referensi struktur, bukan XNU Apple langsung; tulis platform layer RV1103 dari nol.

### A6. Milestone minimal ("hello world" Mach)
1. **Hello physical-mode**: U-Boot memuat ELF; entry assembly setup stack → panggil C; driver UART2 polled mencetak teks. Tanpa MMU, tanpa interrupt. Membuktikan toolchain + loader + console.
2. **MMU/pmap**: aktifkan VMSA ARMv7-A dengan mapping section 1MB; pmap minimal.
3. **Exceptions**: vector table; jalur trap SVC; data-abort → `vm_fault` Mach.
4. **Threads**: context switch + ping-pong dua thread; lalu timer tick (butuh alamat timer dari B3).
5. **IPC smoke test**: satu task kirim message ke task lain — "hello world"-nya microkernel.
6. Baru kemudian: user task loading, `ddb`, driver selain UART.

---

## Bagian B — Luckfox Pico Mini

### B1. Spesifikasi
| Item | Nilai |
|---|---|
| SoC | Rockchip **RV1103** (G1) |
| CPU | Single-core ARM **Cortex-A7 @ 1,2 GHz** (sebagian dokumen menyebut sampai 1,5 GHz) + NEON/FPU; plus MCU RISC-V @ 400 MHz |
| Cache | 32KB I/D L1, 128KB L2 |
| RAM | **64MB DDR2 on-chip (16-bit)** |
| Lainnya | NPU 0,5 TOPS; ISP 4MP@30fps; SRAM sistem 256KB + SRAM PMU 8KB + **MaskROM 20KB** |
| Periferal | UART×4, SPI×1, I2C×3, timer 6ch + 2ch secure, PWM 11–12ch, USB 2.0 OTG Type-C, MIPI CSI 2-lane, Ethernet MAC+PHY 10/100M |
| Board | ~28,4×21 mm; header 2×11 (s/d 17 GPIO); **Mini A = slot microSD**, **Mini B = SPI NAND 128MB**; tombol BOOT; LED ACT/USER; 5V via USB-C |

### B2. Alur boot
Rantai standar Rockchip: **MaskROM (20KB) → idblock (DDR init + MiniLoaderAll/SPL) → U-Boot → boot.img**. SDK resmi `github.com/LuckfoxTECH/luckfox-pico` (`./build.sh lunch` → `./build.sh`) menghasilkan `MiniLoaderAll.bin`, `uboot.img`, `boot.img`. Flashing: tahan tombol **BOOT** sambil colok USB → mode MaskROM; Windows pakai **SocToolKit**, Linux pakai **`upgrade_tool`** Rockchip (closed-source, hanya SPI NAND/eMMC — bukan SD; disebut mendukung Ubuntu 22.04 x86_64). **Untuk eksperimen kernel, U-Boot adalah loader yang natural** — MaskROM tidak perlu disentuh.

### B3. Detail hardware level rendah
- **Debug UART = UART2** (wiki resmi Luckfox).
- **UART2 base = `0xff4c0000`** (dikutip dari device tree SDK Luckfox via README st7305-kernel-drivers — bukan hasil baca TRM, tapi actionable).
- Baud console: **1500000n8** di image bawaan (komit plan44/openwrt, April 2026); sebagian dokumen komunitas menyebut 115200 — **ada perbedaan, coba 1500000 dulu**.
- Interrupt controller: datasheet menyebut programmable interrupt controller yang merutekan semua IRQ ke **GIC** CPU (Cortex-A7 ⇒ kelas GICv2). **Versi GIC dan base address GICD/GICC belum terkonfirmasi** — tidak ada TRM publik RV1103.
- **DRAM physical base belum terkonfirmasi** (sebagian besar SoC Rockchip pakai `0x0`, sebagian `0x60000000` — nilai RV1103 belum terverifikasi).
- Base address timer/CRU: **belum terkonfirmasi** (gap TRM yang sama).
- Nomor pin TX/RX UART2 di header: hanya ada di diagram gambar wiki (tidak machine-readable) — gap.

### B4. Antarmuka debug
- **UART2** via adapter USB-serial **3,3V** — **5V merusak SoC**. Adapter ch341 dilaporkan bekerja.
- Download USB via **mode MaskROM** (tombol BOOT) — jalur recovery selalu tersedia; risiko brick rendah.
- **JTAG**: ada di RV1103 EVB; **belum terkonfirmasi di board Pico Mini**.

### B5. Kerja komunitas
Ekosistem aktif tapi semuanya berorientasi Linux/Buildroot: SDK resmi + RT-Thread untuk MCU RISC-V (`sysdrv/source/mcu/rt-thread`); **plan44/openwrt** target cortexa7 (aktif April 2026); yocto; Nerves; driver kernel komunitas. **Tidak ditemukan proyek bare-metal-from-scratch atau microkernel kustom** — belum ada atau tidak terindeks.

### B6. Kendala untuk microkernel
1. **RAM 64MB bukan kendala** — lega untuk Mach.
2. **Dokumentasi adalah kendala**: alamat GIC, timer, DRAM-base, CRU harus digali dari device tree SDK/U-Boot/OpenWrt. Ini critical path dan harus jadi tugas #1.
3. Single-core: tidak butuh SMP. NEON/FPU bisa diabaikan dulu.
4. Mini B (SPI NAND) flashing-nya lebih ribet; **Mini A (microSD)** iterasinya `dd` dalam hitungan detik.
5. MCU RISC-V abaikan dulu (jalan firmware RT-Thread sendiri).
6. Console 1500000 baud non-standar — adapter USB-serial harus mendukungnya (kebanyakan ch341/cp210x bisa).

---

## Roadmap yang disarankan

1. **Siapkan Pico Mini A** + adapter serial 3,3V di UART2; konfirmasi console 1500000n8 di image bawaan.
2. **Ekstrak kebenaran hardware**: ambil base address GICD/GICC, timer, dan DRAM dari device tree SDK Luckfox / sumber U-Boot / DTS plan44-openwrt — **sebelum** menulis kode kernel.
3. **Bare-metal "hello world"** yang dimuat U-Boot (physical mode, UART2 polled) untuk validasi toolchain + alamat.
4. Kerjakan milestone A6 dengan **darwin-on-arm ARMv7** sebagai referensi struktur; tulis platform layer RV1103 dari nol.
5. Jangan sentuh MaskROM; selalu simpan image microSD yang sudah terbukti jalan untuk recovery.

## Yang belum terverifikasi (open questions)
- Versi & base address GIC (GICD/GICC), base timer/CRU, DRAM physical base — butuh penggalian device tree.
- Nomor pin TX/RX UART2 di header.
- Ketersediaan JTAG/SWD di board Pico Mini.
- Entri "ARM32" di infobox Wikipedia Mach.
- Clock CPU 1,2 vs 1,5 GHz (pakai 1,2 GHz sebagai angka aman).
- Baud console 1500000 vs 115200.

## Sumber utama
- `github.com/darwin-on-arm/xnu` — referensi bring-up ARMv7 Mach-family terdekat
- `github.com/bugaevc/gnumach` (branch `wip-aarch64`) — port AArch64 GNU Mach 2024
- `github.com/LuckfoxTECH/luckfox-pico` — SDK resmi Luckfox
- `wiki.luckfox.com/Luckfox-Pico-Plus-Mini` — wiki resmi (satu-satunya yang diverifikasi live)
- Laporan lengkap riset (20KB, Inggris): `~/workspace/research_notes/mach-kernel-research-20260926-0540/report.md`

*Catatan metodologi: sebagian besar temuan berbasis indeks pencarian (bukan render live di browser); fakta yang akan dipakai untuk aksi (UART2 base, boot chain, flashing tools) didukung minimal dua sumber yang sepakat, kecuali yang ditandai belum terverifikasi di atas.*
