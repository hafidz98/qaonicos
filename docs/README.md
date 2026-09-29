# Dokumentasi QaonicOS

Satu pintu untuk semua dokumen QaonicOS (kode: `~/workspace/qaonic_os/`).

## Dokumen teknis kernel (repo)

| File | Isi |
|---|---|
| `SYSCALL-ABI.md` | ABI syscall (per fase/app, termasuk 60–72 untuk display + jam/UDP) |
| `MIGRASI-MACH3.md` | Migrasi kernel Mach 3 (Fase A–D) |
| `ANALISIS-PORTING.md` | Analisis porting Mach ke Luckfox |
| `ROADMAP-FASE13-18.md` | Roadmap fase kernel |
| `hw-addrs.md` | Alamat hardware |
| `assets/` | Screenshot (lihat `assets/screenshots/`) |

## Rencana & riset

| Folder | Isi |
|---|---|
| `rencana-app-qaonicos/` | Rencana implementasi aplikasi device (fase A0–A4 + status) |
| `rencana-face-qaonicos/` | Rencana face UI / Qabot |
| `riset-ui-qaonicos/` | Riset UI |
| `riset-agent-runtime-harness/` | Riset agent runtime harness (RINGKASAN.md) |
| `riset-fido2-esp32c3/` | Riset FIDO2 di ESP32-C3 |
| `riset-mach-kernel-luckfox/` | Riset kernel Mach untuk Luckfox |
| `ringkasan-a4-ntp-persist/` | Ringkasan App A4 (jam NTP + persistensi NVS) |
| `dashboard-qaonicos.html` | Snapshot dashboard System Monitor (hasil ukur QEMU) |

## Screenshot (`assets/screenshots/`)

- `ss-qaonicos-dashboard*.png`, `ss-qaonicos-metrics.png` — dashboard HTTP Fase 12
- `qabot-a1-qemu.png` — App A1: Qabot di QEMU
- `qabot-a2-*.png` — App A2: face server + uiapp (menu, monitor, settings, power, sleep/wake)
- `qabot-a3-*.png` — App A3: layar WiFi/BLE/LLM/Passkey
- `qabot-a4-face.png`, `qabot-a4-menu.png` — App A4: Qabot + menu/status bar
- `qabot-face-v1.png` — prototipe wajah awal
