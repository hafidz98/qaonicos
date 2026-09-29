# Ringkasan App A4 — Jam NTP + Persistensi NVS (QaonicOS)

**Status:** DONE 2026-09-29 · Branch `app/a4-ntp-persist` · Commit `44a2c5f`

## Jam dinding via NTP

- **UDP di kernel** (`kernel/kernel/arm/netstack.c`): `netstack_udp_send/recv`,
  demux IP_UDP, slot RX tunggal, ephemeral port 0xC000+.
- **Syscall 69–72** (`SYS_TIME_SET/GET`, `SYS_UDP_SEND/RECV`) + wrapper ulib
  + dokumentasi `docs/SYSCALL-ABI.md`.
- **Daemon `ntp`** (progid 2, scheduler NDAEMON 2→3): resolve `pool.ntp.org`
  via DNS ke 10.0.2.3:53, query SNTP ke server:123, `sys_time_set()`.
  Retry 3×3 detik, resync tiap 3600 detik, tolak timestamp basi (< 2023).
- **Status bar uiapp**: tampilkan jam WIB (UTC+7) bila `TIME_GET ≠ 0`,
  fallback ke uptime bila belum sinkron.
- **Mock co-MCU** `TIME.GET` → ISO8601 jam real (label `MOCK` bila jam
  kernel belum diset).

## Persistensi setting (NVS)

- `tools/mkfat32.py`: reserve cluster 3 sebagai BAD → area NVS di **LBA 552**
  (2 sektor); saat format ulang, 2 sektor di-preserve byte-identik bila
  magic `QNVS` cocok.
- `user/cfg/`: `struct nvs_state` (868 byte: magic, versi, CRC-16/CCITT,
  8 KV, state BLE, 2 WiFi tersimpan), `cfg_load`/`cfg_save`, `NVS_SECTOR 552`.
- `mock_comcu.c` refactor ke `nvs_state` sebagai backing store dengan
  write-through `cfg_save` (KV.SET/DEL LLM, WiFi, BLE).
- `build-md.sh`: link `cfg.o` ke uiapp (54606 < 65536 byte).

## Verifikasi

| Uji | Hasil |
|---|---|
| Unit test host logika DNS/SNTP (`dns_resolve`, `sntp_sync`) | SEMUA OK (query valid, parse terkompresi, konversi NTP→Unix tepat, timestamp basi ditolak) |
| Jalur RX kernel (demux → slot → `SYS_UDP_RECV`) | OK (injeksi loopback sementara, lalu dihapus) |
| Jalur TX (ARP + kirim) | OK (terlihat di log guest) |
| Persistensi 2 boot | OK (`WRITTEN` → reboot + format ulang → `PERSIST OK key0=persist.probe val0=A4-OK`) |
| Boot penuh | 8/8 program, 17/17 file PASS |
| HTTP | 200 |

## Batasan jujur

Sandbox VM memblokir **semua** `sendto` UDP (bahkan loopback), sehingga
sinkron NTP asli tidak bisa end-to-end di lingkungan ini — daemon retry
graceful tiap 60 detik dan status bar menampilkan fallback uptime
(lihat `qabot-a4-menu.png`: `00:00:50`). Di jaringan normal, paket guest
ke 10.0.2.2/10.0.2.3 diteruskan slirp QEMU seperti biasa.

## Screenshot

- `qabot-a4-face.png` — Qabot (mata squircle, kondisi terbuka)
- `qabot-a4-menu.png` — Menu + status bar (fallback uptime)
