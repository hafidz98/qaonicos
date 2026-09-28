# QaonicOS Syscall ABI (Fase B, kernel Mach 3)

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
| 20 | `SYS_WRITE` | r0=fd (1/2 konsol), r1=buf, r2=len | byte tertulis / `-1` | Tulis ke UART. Buffer harus dalam window VA user `[0x100000, 0x130000)` |
| 21 | `SYS_YIELD` | — | `0` | Kooperatif: tunggu 1 tick timer 100 Hz (wfi, IRQ hidup) |
| 22 | `SYS_EXIT` | r0=code | tidak kembali | Akhiri task user; kernel lanjut halt |
| 24 | `SYS_SBRK` | r0=inkremen byte | brk lama / `(void*)-1` | `incr=0` = query. Heap 64 KB pre-alloc per task |

## Nomor dicadangkan (Fase C/D)

| Rentang | Peruntukan | Status |
|---|---|---|
| 10–12 | `SYS_SEND` / `SYS_RECV` / `SYS_RPC` (IPC) | Fase D |
| 23 | `SYS_RPC_USER` | Fase D |
| 30–34 | `SYS_OPEN` / `READ` / `CLOSE` / `LS` / `DELETE` (ramfs) | Fase C |
| 40–41 | `SYS_GPIO_SET` / `SYS_GPIO_GET` | Fase D |
| 50–51 | `SYS_SD_READ` / `SYS_SD_WRITE` | Fase D |
| 52–56 | `SYS_MKDIR` / `FAT_WRITE` / `FAT_READ` / `FAT_DELETE` / `READDIR` | Fase D |
| 57–59 | `SYS_STAT` / `SYS_TLIST` / `SYS_READ_CONSOLE` | Fase D |

## Layout memori user (Fase B)

| VA | Isi |
|---|---|
| `0x100000` | Kode init (di-embed dari `user/init.c`, N halaman) |
| `0x110000` | Stack user 1 halaman (SP awal `0x111000`) |
| `0x120000`–`0x130000` | Heap sbrk, 16 halaman (64 KB), zeroed |

Task user berjalan di **USR mode (PL0)**, genuinely unprivileged.
Kernel tetap ter-map (identity, priv-only) di L1 user sehingga trap
handler jalan normal; fault user (abort/undef) hanya membunuh task
tersebut, bukan panic kernel.

## Batasan Fase B (jujur)

- Validasi buffer `SYS_WRITE` = cek range VA, bukan walk page table.
- Satu task user (init); `brk` disimpan per-task di `struct user_task`
  (siap multi-task di Fase C).
- `SYS_YIELD` menunggu 1 tick; tidak ada penjadwalan antar task user
  (scheduler masih kooperatif).
- `hello.c` hanya cek build; yang diluncurkan saat boot adalah `init`.
