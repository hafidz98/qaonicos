# Rencana Lanjutan (tanpa board) — K1..K8

Track lanjutan QaonicOS, perintah Hafidz 2026-10-27: selesaikan 8 item
backlog yang bisa dikerjakan tanpa board, SEBELUM revisi UI berikutnya.
Urutan di bawah mengikat kecuali Hafidz mengubahnya. Satu item = satu
worktree + branch `dev/k<N>` = satu merge atomik + push oleh koordinator,
dengan baterai + gate visual (bila menyentuh UI) per item.

Aturan standing yang berlaku: upstream TIDAK PERNAH diedit; daemon order
load-bearing (face=0, uiapp=1, qabotd=2, uipcld=3, linkd=4, ulink5d=5,
sh=6 last daemon image, spawn slot=7, NDAEMON 8); linkd TIDAK BOLEH
tumbuh (94,0% embed cap); SYS_STAT tidak diubah semantiknya; baterai
full per item; flake SELALU rerun standalone sebelum disebut regresi
(FND1 ev=12, C6L5 death-window, one-shot SD/fat timing, ONB2 finale,
SHELL `ls` /sd = race probe blk, UI5 long-burst = pacing chunked).

## K1 — FPU/NEON (THR-1, prioritas tertinggi)

Fakta: VFP/NEON di-enable tapi `fpu_save`/`fpu_restore` stub kosong.
Tujuan: save/restore konteks FPU/NEON per thread yang benar (kunci pada
identitas thread, bukan mode frame — pelajaran Fase 10), dibuktikan
dengan thread yang benar-benar memakai register VFP/NEON bergantian
tanpa korup. Risiko: salah save/restore = korup diam-diam; verify harus
membandingkan nilai register, bukan sekadar boot PASS.

## K2 — qabotd mendaftar nama IPC `tools`

Fakta: linkd meneruskan peer DISPATCH → IPC `tools` (SYS_RPC 12,
id 0x3101, timeout 5 s); selama ini forwarding HANYA terverifikasi
lawan stub `ulinktools` karena qabotd belum mendaftar `tools`.
Tujuan: qabotd mengklaim nama `tools` via identitas spawn kernel
(D-IPC-2), melayani TOOL.DISPATCH end-to-end lewat linkd, dan baterai
C6L5 diulang lawan qabotd beneran (bukan stub). Risiko: jangan ubah
kontrak LINKD-CONTRACT; qabotd tidak boleh mengeksekusi perintah
tanpa policy gate (Q1–Q9).

## K3 — Retry/robustness probe virtio-blk

Fakta: `machine_init` → `blk_init` (probe slot virtio-mmio) →
`fat32_mount` (caller di `md/arm/machdep.c`). Saat race kalah (SD tak
ter-probe, `nblkdevs=1`), semua syscall FAT gagal dan hanya check
`ls` SHELL yang kena; image FAT sehat (header/FAT/root valid).
Tujuan: probe blk diulang/robust sehingga mount tidak kalah race
(mis. retry probe per slot / ulangi negosiasi yang gagal), dibuktikan
dengan N boot berturut-turut tanpa `ls /sd` gagal karena probe.
Risiko: jangan ubah pemetaan dev 0 = internal / dev 1 = SD (identifikasi
lewat signature FAT32, bukan urutan slot — pelajaran Fase 15).

## K4 — Respawn FND-3 mencakup face

Fakta: FND-3 (progress stamps, deadline, respawn budget 2/daemon/boot,
bootlog event 13–16, class-(b) detector) TIDAK mencakup face (daemon
idx 0): kalau face mati, tidak di-respawn.
Tujuan: face masuk skema respawn yang sama (atau skema yang dibuktikan
setara), termasuk token display + status face request yang bersih
saat respawn. Risiko: face adalah pemilik display; respawn yang salah
bisa merebut token dari uiapp atau meninggalkan layar menggantung —
verify harus mencakup skenario face mati saat uiapp memegang display
dan sebaliknya.

## K5 — Wiring status radio live ke ikon status bar

Fakta: ikon status bar (Material Symbols, UI-8) saat ini default
OFF/OFF di QEMU; `uilvgl_set_radio_icons(wifi_on, bt_on)` di `lv_port.c`
adalah satu-satunya call site; status radio tersedia lewat linkd
(`uproto` WIFI.STATUS/BLE.STATUS — panggilan yang sama dipakai layar
wifi/ble). Wiring langsung DITUNDA di UI-7/UI-8 karena RPC blocking di
jalur build menu akan stall tampilan menu saat linkd bermasalah.
Tujuan: desain poll/cache (mis. cache + refresh periodik di luar jalur
build menu, atau EVT dari linkd) + implementasi + verify; di QEMU tanpa
radio, ikon tetap OFF secara jujur. Risiko: jangan stall menu; jangan
klaim state yang tidak ada.

## K6 — Drain PL011 RX FIFO vs scheduling kooperatif

Fakta: input serial shell flaky di bawah beban (PL011 RX FIFO drain vs
scheduling kooperatif — follow-up level driver). Mitigasi harness sudah
ada (injection verification, `shell_cmd_chunked` pacing), tapi akar
driver belum diperbaiki; kelas flake ini juga di balik stall PIN UI-3
dan burst loss UI-5.
Tujuan: driver menguras RX FIFO secara andal di bawah beban (tanpa
mengubah semantik input), dibuktikan dengan burst panjang + beban
bersamaan tanpa byte hilang. Risiko: jangan ubah perilaku normal;
perubahan driver serial menyentuh semua verify script — baterai full
wajib hijau.

## K7 — Kebocoran daemon-lifecycle (fd-table ramfs + task/vm_map/pmap)

Fakta: IPC-3 memilih teardown SURGICAL di `daemon_reclaim` (bukan
`task_terminate`) dengan kebocoran yang diketahui dan dibatasi budget
FND-3: task/vm_map/pmap leak + ramfs fd-table leak.
Tujuan: tutup kebocoran-kebocoran itu (atau buktikan batasnya dengan
soak spawn/exit berulang + ukur), tanpa mengubah kontrak respawn
FND-3. Risiko: area ini menyentuh Mach VM/task — perubahan harus
minimal dan dibuktikan soak, bukan sekadar boot PASS.

## K8 — Akar masalah VM free-count (Monitor Memory mustahil)

Fakta: SYS_STAT menghitung `mem_used_kb = (total_pages -
vm_page_free_count) * 4`; nilai tampil 199872/65536 KB mengimplikasikan
`vm_page_free_count` = −33584 (signed). Counter di upstream
`kernel/vm/vm_resident.c:347` (simbol BSS tunggal); bootstrap waras
(10152 free pages); akuntansi MI + `pmap_startup` ARM seimbang pada
inspeksi; probe UI-5 (40 grant cycle + spawn burst + 200-key burst)
stabil di 29872 KB, nol drift. Diduga butuh uptime panjang / loop
qabot / volume web-IPC/linkd berat, atau one-off langka. UI-5
menyatakan ini rabbit hole VM dan berhenti; guard jujur (`-` bila
used > total) adalah mitigasi yang shipped.
Tujuan: temukan akar (atau buktikan dengan soak yang direproduksi:
uptime panjang, loop qabot, volume IPC berat) dan perbaiki; bila
tidak ditemukan dalam batas yang wajar, dokumentasikan hasil soak +
guard tetap. Risiko: JANGAN ubah semantik SYS_STAT untuk qfetch/web
tanpa alasan; jangan kejar tanpa batas — item ini boleh berakhir
"terdokumentasi, guard dipertahankan" bila soak tidak mereproduksi.

## Setelah K1..K8: revisi UI

Revisi UI berikutnya dimulai SETELAH semua K1..K8 landing (atau
dinyatakan selesai/terdokumentasi), sesuai perintah Hafidz 2026-10-27.
