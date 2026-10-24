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
| 69 | `SYS_TIME_SET` | r0=unix_sec | `0` | App A4: set jam dinding kernel (detik Unix UTC). Dipakai daemon `ntp` setelah sinkron NTP |
| 70 | `SYS_TIME_GET` | — | detik Unix / `0` | App A4: jam dinding = nilai SET + (ticks berlalu)/100. `0` bila belum pernah di-set (fallback: `SYS_UPTIME`) |
| 71 | `SYS_UDP_SEND` | r0=dst_ip, r1=dst_port, r2=buf, r3=len (≤512) | `0` / `-1` | App A4: kirim datagram UDP (proto 17) via `netstack_ip_send`. Source port ephemeral kernel (49152+). `-1` bila MAC dst belum dikenal (ARP dikirim; coba lagi) |
| 72 | `SYS_UDP_RECV` | r0=buf, r1=maxlen (≤512), r2=&src_ip, r3=&src_port | n / `0` / `-1` | App A4: ambil datagram balasan (non-blocking). Slot RX tunggal; hanya paket yang dport-nya = port ephemeral kiriman terakhir yang disimpan. `0`=belum ada |
| 73 | `SYS_TCP_CONNECT` | r0=dst_ip, r1=dst_port | `0` / `-1` | Q2a: buka koneksi TCP client (SYN). Satu koneksi client dalam satu waktu; `-1` bila sibuk |
| 74 | `SYS_TCP_STATUS` | — | `0`..`3` | Q2a: 0=CLOSED, 1=SYN_SENT, 2=ESTABLISHED, 3=FIN_SENT |
| 75 | `SYS_TCP_SEND` | r0=buf, r1=len (≤1200) | n / `0` / `-1` | Q2a: kirim (stop-and-wait per segmen). `0`=coba lagi (belum di-ack / ARP); `-1`=tak tersambung |
| 76 | `SYS_TCP_RECV` | r0=buf, r1=maxlen (≤4096) | n / `0` | Q2a: baca buffer RX; `0`=belum ada data |
| 77 | `SYS_TCP_CLOSE` | — | `0` | Q2a: kirim FIN (active close) |

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

## Syscall power (FND-1 ✅)

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 106 | `SYS_RESET` | — | tidak kembali / `-1` | Reset software via PSCI `SYSTEM_RESET` (QEMU: conduit HVC). Sebelum reset, kernel mencatat `last_reason=4` (software request) + event bootlog 12. RV1103: seam TODO(HW) di `psci.c` (PMU/WDT) |
| 107 | `SYS_POWEROFF` | — | tidak kembali / `-1` | Power off via PSCI `SYSTEM_OFF`. Pencatatan reason sama seperti `SYS_RESET` |

## Syscall IPC (IPC-1..IPC-4 ✅, `user/ulib/uipc.h` v1 frozen)

> Language note (2026-10-24): sections added from IPC-5 on
> follow the English-artifact rule; the older sections of
> this document predate it and are pending translation.
> The IPC surface below is complete and authoritative for
> the frozen `uipc.h` v1 contract (see also
> `docs/rencana-ipc/DEVLOG-IPC4.md` §7).

Mach IPC, built on the real upstream `ipc/` machinery
through the extension-layer glue (`mach3-port/src/ext/
ipc_glue.c`). Identity is the caller's own Mach task
(`cur_utask`); `mach_msg_trap` is never called (it
would take the boot thread's identity). A message is a
24-byte header plus a flat inline body, body cap
**4096 B** (owner decision D-IPC-3), total max 4120 B,
wire-rounded up to a multiple of 4 with zero padding.
COMPLEX is never set: bodies are never parsed.
Sends never block: a send to a full queue returns
`UIPC_EFULL` immediately.

| No | Nama | Argumen | Kembali | Keterangan |
|---|---|---|---|---|
| 10 | `SYS_SEND` | r0=dest name, r1=buf, r2=len (total, header+body), r3=flags | `0` / `UIPC_E*` | Send a message. Flag bit 0 (`UIPC_F_SEND_ONCE`): dest is a send-once right, moved (the reply path). `hdr.reply` = a reply port in the sender's space (0 = no reply wanted); the glue turns it into a send-once right for the receiver. `hdr.id` carried verbatim |
| 11 | `SYS_RECV` | r0=port, r1=buf, r2=max, r3=timeout_ms | total len / `UIPC_E*` | Receive. Timeout encoding (IPC-2): `0` = poll (never blocks, empty → `UIPC_EEMPTY`); `>0` = park up to that many ms (expiry → `UIPC_ETIME`); `-1` = park without a deadline (allowed, bootlogged event 17; the watchdog treats a parked daemon as exempt-but-observed). A parked waiter wakes on send to its port; re-dispatch verdict order is: queued message → peer-died `UIPC_EPEER` → deadline `UIPC_ETIME` → park again. On receive, `hdr.reply` = the send-once reply right in the receiver's space (0 = sender wanted no reply); `hdr.dest` = the port the message arrived on |
| 12 | `SYS_RPC` | r0=dest name, r1=buf (request in, reply out, same buffer), r2=len (request length = reply budget), r3=timeout_ms | reply len / `UIPC_E*` | RPC engine (IPC-4). The wrapper allocates a fresh reply port, sets `hdr.reply`, and destroys the reply port on every return path. Server answers with `uipc_reply()` to the send-once name; a second reply on the same send-once name fails. Server death mid-RPC resolves to `UIPC_EPEER`; a reply larger than the budget returns `UIPC_E2BIG` and the message stays queued (Mach semantics) |
| 23 | `SYS_RPC_USER` | r0=name string, r1=buf, r2=len, r3=timeout_ms | reply len / `UIPC_E*` | Registry lookup, then the `SYS_RPC` engine. A delivered notify id 71 (Mach send-once death notice) is mapped to `UIPC_EPEER`; reply ids must never be 71 |
| 25 | `SYS_IPC_PORT` | r0=sub, r1=a1, r2=a2 | name / `0` / `UIPC_E*` | Port management. Sub-ops: `0` ALLOC_RECV → new receive port name; `1` DESTROY (a receive name destroys the port, discarding its queue); `2` SETQLIMIT (name, limit 1..16; default 5, Mach `MACH_PORT_QLIMIT_DEFAULT`; a 16-deep queue at the 4096 B cap is ~66 KB of kmsg zone per port); `3` REGISTER (name string ≤15 chars, port name) → `0`; `4` LOOKUP (name string) → send name; `5` REG_GEN (name string) → generation. Subs ≥3 take a kernel-copied name string in a1 |

Registry (IPC-4): names live in the kernel (8-entry
table), so the registry survives a registered daemon's
death and answers lookups during its respawn window.
A re-registration after death/respawn bumps the
generation (fresh registration = 1); a lookup on a
name whose owner died returns `UIPC_EDEST` — the
registry never mints a right to a dead port (upstream
`ipc_port_make_send()` asserts the port active).
Reserved system names may be claimed only by the
owning program, enforced by the kernel's own spawn
identity of the caller slot, never by a
caller-supplied id: `tools` (owner: qabotd,
daemon_images index 2), `link` (unbound until C6L-5
binds linkd; any claim refused until then). A
non-owner claim returns `UIPC_EPERM` and has no side
effect. An evicted dead entry re-registered later
restarts at generation 1 (IPC-5 backlog note: C6L
clients must treat generation as "changed since last
lookup", not as a monotonic clock across eviction).

Port lifecycle (IPC-3): when a daemon dies (clean
exit, fault, or watchdog kill), `daemon_reclaim`
destroys every right in the dead task's space (receive
rights first, queues discarded) and completes parked
waiters: a waiter on a destroyed port wakes with
`UIPC_EPEER` (bootlog event 18 records the death).
Teardown is surgical, not `task_terminate` (see
DEVLOG-IPC3): task objects / `vm_map` / `pmap` still
leak on death, bounded by the FND-3 respawn budget.

Error table (`UIPC_E*`, negative returns; `-2` stays
the dispatch's ENOSYS for unimplemented numbers and
is deliberately never reused here): `-1` EINVAL (bad
argument / wrong right kind), `-3` EDEST (dead port /
no such name or right), `-4` EFULL (send: queue full,
poll), `-5` EEMPTY (recv: queue empty, poll), `-6`
E2BIG (too large; on recv the message stays queued),
`-7` EBUSY (registry: live entry owned by a different
caller, or table full of live entries), `-8` ENOMEM
(kernel resource shortage), `-9` ENORV (registry:
name never registered), `-10` ETIME (blocking recv:
deadline expired with the queue still empty), `-11`
EPARK (internal park/wake handshake, never returned
to userspace), `-12` EPEER (blocking recv: the peer
port died while or before the wait), `-13` EPERM
(registry: reserved name claimed by a non-owner, or
an unbound reserved name claimed by anyone).

## Syscalls implemented but not yet backfilled here

The following numbers exist in the dispatch
(`mach3-port/src/ext/user.c` defines, verified
2026-10-24) and are documented across their track
plans and dev logs, but their full rows are not yet
written into this document. Their names below are
exactly the define names — semantics live in the
cited tracks, not restated here:

| No | Define | Track |
|---|---|---|
| 78 | `SYS_FACE_EXPR` | Qabot face (Q4) |
| 79, 80 | `SYS_SPAWN`, `SYS_SPAWN_WAIT` | Q9 spawn |
| 82–88 | `SYS_AUDIO_*` (INIT, REG, START, READ, STOP, GAIN, OVERRUN) | Audio D1–D4 |
| 89–92 | `SYS_PL330_*` (INIT, RX, UCLEN, UCBYTE) | PL330 stage 1 |
| 93–97 | `SYS_LFS_*` (RUNTEST, MOUNT, LIST, READ, WRITE) | B1 littlefs |
| 98 | `SYS_BOOTSTATE` (subs 0–11) | B2/B3/B4, FND-5 |
| 99 | `SYS_BOOTLOG` | B2 |
| 100, 101 | `SYS_TIME_QUALITY`, `SYS_TIME_SYNC` | B3 NTP gating |
| 102 | `SYS_GUI_START` | Boot UX |
| 103 | `SYS_WEBUI` | ONB-1 |
| 104 | `SYS_CRED` | B5.5 credential store |
| 105 | `SYS_EVENT_INJECT` | Button injection (dev/test) |

## Nomor dicadangkan

| Rentang | Peruntukan | Status |
|---|---|---|
| — | — | Nothing reserved: the IPC numbers (10–12, 23, 25) are implemented since IPC-1..IPC-4; see the IPC section above. Future numbers are allocated by append (principle P2). |

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
