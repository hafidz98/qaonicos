# QaonicOS Syscall ABI (Fase B/C, kernel Mach 3)

Konvensi pemanggilan (ARM, EABI-like):

| Elemen | Keterangan |
|---|---|
| Nomor syscall | `r7` |
| Argumen | `r0`–`r5` |
| Nilai balik | `r0` |
| Instruksi trap | `svc #0` |
| Error | nilai negatif: `-1` gagal umum, `-2` ENOSYS (nomor tak dikenal) |

Nomor **kontinu dengan kernel lama** (Fase 1–17) bila tidak bentrok.
Nomor lama Mach 3 port (M4: `1`=WRITE, `2`=EXIT) dimigrasikan ke
nomor di bawah pada Fase B.

## Syscall inti (Fase B ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 20 | `SYS_WRITE` | r0=fd (1/2 konsol, ≥3 file), r1=buf, r2=len | byte tertulis / `-1` | Tulis ke UART / ramfs. Buffer harus dalam window VA user `[0x100000, 0x130000)` |
| 21 | `SYS_YIELD` | — | `0` | Kooperatif: tunggu 1 tick timer 100 Hz (wfi, IRQ hidup) |
| 22 | `SYS_EXIT` | r0=code | tidak kembali | Akhiri task user; kernel lanjut ke program berikutnya |
| 24 | `SYS_SBRK` | r0=inkremen byte | brk lama / `(void*)-1` | `incr=0` = query. Heap 64 KB pre-alloc per task |

## Syscall file + ramfs (Fase C ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 30 | `SYS_OPEN` | r0=path, r1=flags | fd ≥3 / `-1` | Flag: `O_RDONLY=0`, `O_WRONLY=1`, `O_RDWR=2`, `O_CREAT=0x40` |
| 31 | `SYS_READ` | r0=fd, r1=buf, r2=len | byte / `-1` | fd 0 → 0 (EOF); fd 1/2 → `-1`; fd ≥3 → ramfs |
| 32 | `SYS_CLOSE` | r0=fd | `0` / `-1` | fd 0/1/2 → `-1` |
| 33 | `SYS_LS` | r0=buf, r1=max | jumlah file / `-1` | Format `"nama\n"` per file |
| 34 | `SYS_DELETE` | r0=path | `0` / `-1` | fd terbuka ke file tsb jadi basi (gen counter) |

ramfs: flat, 16 file × 64 KB, tabel fd per-task (16 fd, diindeks
pointer Mach task). Sinkronisasi via `splhigh()`/`splx()`.

## Syscall monitor (Fase C ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 57 | `SYS_STAT` | r0=buf, r1=len (≥36) | `0` / `-1` | Isi `struct qaon_stat` (lihat bawah) |
| 58 | `SYS_TLIST` | r0=buf, r1=max_entri | jumlah / `-1` | Isi array `struct qaon_tentry` |
| 59 | `SYS_READ_CONSOLE` | — | byte 0–255 / `-1` | UART non-blocking (polled) |

## Syscall display (App A1 ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 60 | `SYS_DISPLAY_INFO` | r0=buf, r1=len (≥16) | `0` / `-1` | Isi `struct qaon_display`: `width`, `height` (240×240), `bpp` (16), `flags`. `-1` bila display tak ada |
| 61 | `SYS_DISPLAY_FLUSH` | r0=x, r1=y, r2=w, r3=h, r4=pixels, r5=nbytes | `0` / `-1` | Kirim strip RGB565 ke layar via virtio-gpu (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH). nbytes ≥ w·h·2. **App A2: ditolak (`-1`) bila pemanggil bukan pemegang token display** |
| 62 | `SYS_DISPLAY_GRANT` | — | `0` / `-1` | face (progid 0) memberi token display ke uiapp. `-1` bila pemanggil bukan face |
| 63 | `SYS_DISPLAY_ACQUIRE` | — | `1` / `0` | `1` bila pemanggil adalah pemegang token saat ini |
| 64 | `SYS_DISPLAY_RELEASE` | — | `0` / `-1` | Pemegang token mengembalikan ke face. `-1` bila bukan pemegang |
| 65 | `SYS_DISPLAY_GET_EVENT` | — | `EV_*` / `-1` | Ambil 1 event input dari antrean (16 entri). `-1` bila bukan pemegang token |
| 66 | `SYS_DISPLAY_STATUS` | — | progid | progid pemegang token display (`0`=face, `1`=uiapp) |
| 67 | `SYS_UPTIME` | — | ms | Uptime kernel dalam milidetik (dipakai pacing frame) |
| 68 | `SYS_DISPLAY_SLEEP` | r0=req | `0` / `1` / `-1` | r0=1: pemegang token minta sleep (flag). r0=0: face mengambil+clear flag (`1`=ada permintaan). `-1` bila bukan yang berhak |

### Event input (`EV_*`, via 65)

Console (UART) diterjemahkan kernel menjadi event: `W/A/S/D` = panah
atas/kiri/bawah/kanan, `Enter` = OK, `Esc` = BACK, `M` = MENU; sequence
escape `ESC [ A/B/C/D` juga didukung. Antrean 16 entri, non-blocking
(`EV_NONE`=0 bila kosong). Hanya pemegang token yang menerima event;
saat GRANT/RELEASE antrean dikosongkan agar tidak ada event basi.

### Protokol token display (App A2)

Satu token, dua daemon: `face` (progid 0, pemilik default) dan `uiapp`
(progid 1). `face` me-render Qabot hanya saat memegang token; tombol
MENU (`M`) → `SYS_DISPLAY_GRANT` → uiapp me-render menu/settings/
monitor/power. Kembali ke Qabot via `SYS_DISPLAY_RELEASE` (BACK di menu
root, idle 30 dtk di uiapp, atau Power → Sleep Now). `SYS_DISPLAY_FLUSH`
dari non-pemegang ditolak kernel — tidak ada balapan gambar.

`struct qaon_stat` (36 byte): `uptime_ms` (real, tick×10),
`cpu_pct`, `mem_used_kb` (real), `mem_total_kb` (real, 65536),
`blk_total_sec` (real), `blk_used_sec`, `net_rx_kb`, `net_tx_kb`,
`nthreads` (real). Field yang belum tersedia diisi
`QAON_UNKNOWN` (`0xFFFFFFFF`): `cpu_pct` (belum ada idle accounting),
`blk_used_sec` (belum ada FS di disk); net = 0 (belum ada driver).

`struct qaon_tentry` (12 byte): `id`, `state` (`0`=RUNNABLE,
`2`=EXITED), `user` (`1`). Hanya task user yang diluncurkan saat
boot (init, ucat, uls, uecho, umon); thread kernel/MI belum
terdaftar (Fase D).

## Syscall GPIO (Fase D ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 40 | `SYS_GPIO_SET` | r0=pin, r1=value (0/1) | `0` / `-1` | Set pin GPIO (bank 0). QEMU: mock RAM; RV1103: register asli |
| 41 | `SYS_GPIO_GET` | r0=pin | `0`/`1` / `-1` | Baca pin GPIO |

## Syscall SD card (Fase D ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 50 | `SYS_SD_READ` | r0=lba, r1=buf, r2=nsectors | `0` / `-1` | Baca sektor dari SD (dev 1) |
| 51 | `SYS_SD_WRITE` | r0=lba, r1=buf, r2=nsectors | `0` / `-1` | Tulis sektor ke SD (dev 1) |

## Syscall FAT32 (Fase D ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 52 | `SYS_MKDIR` | r0=path | `0` / `-1` | Buat direktori di `/sd` (path wajib prefix `/sd`) |
| 53 | `SYS_FAT_WRITE` | r0=path, r1=buf, r2=len | byte / `-1` | Tulis file ke FAT32 |
| 54 | `SYS_FAT_READ` | r0=path, r1=buf, r2=len | byte / `-1` | Baca file dari FAT32 |
| 55 | `SYS_FAT_DELETE` | r0=path | `0` / `-1` | Hapus file/direktori kosong |
| 56 | `SYS_READDIR` | r0=path, r1=buf, r2=max | entri / `-1` | List direktori |

## Nomor dicadangkan

| Rentang | Peruntukan | Status |
|---|---|---|
| 10–12 | `SYS_SEND` / `SYS_RECV` / `SYS_RPC` (IPC) | belum |
| 23 | `SYS_RPC_USER` | belum |

## Layout memori user (Fase B/C)

| VA | Isi |
|---|---|
| `0x100000` | Kode program (di-embed, N halaman; tiap program task sendiri) |
| `0x110000` | Stack user 1 halaman (SP awal `0x111000`) |
| `0x120000`–`0x130000` | Heap sbrk, 16 halaman (64 KB), zeroed |

Program user diluncurkan **berurutan** saat boot: `init` (setup
ramfs) → `ucat` → `uls` → `uecho` → `umon` (one-shot). Koordinasi
antar program via file sentinel ramfs (tanpa argv/spawn — shell
paling akhir). Tiap task berjalan di **USR mode (PL0)**, genuinely
unprivileged. Kernel tetap ter-map (identity, priv-only) di L1 user
sehingga trap handler jalan normal; fault user (abort/undef) hanya
membunuh task tersebut, bukan panic kernel.

## Batasan Fase C (jujur)

- Validasi buffer = cek range VA, bukan walk page table.
- `SYS_YIELD` menunggu 1 tick; tidak ada preemption antar task user
  (scheduler masih kooperatif; program jalan satu per satu).
- Trampoline `user_exit_trampoline` me-restore **semua** register
  callee-saved (r4–r11) — bug Fase C awal hanya me-restore r4,
  merusak variabel compiler di r6 setelah program pertama exit.
- `hello.c` hanya cek build; tidak diluncurkan saat boot.
