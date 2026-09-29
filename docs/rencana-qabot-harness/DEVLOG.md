# DEVLOG — Qabot Harness Q1

Format: `## YYYY-MM-DD HH:MM — judul` + butir singkat (apa, hasil, keputusan).

## 2026-09-29 10:45 — setup track

- Aturan kerja baru dari Hafidz dicatat di AGENTS.md: selalu riset +
  `docs/plan` dulu, dev logs agar tertrack rapi (standing).
- Track Q1 dibuka: `docs/rencana-qabot-harness/PLAN.md` ditulis dari
  `docs/riset-agent-runtime-harness/RINGKASAN.md` §6–§7.
- Watchdog didaftarkan: `qaonic-qabot-q1-harness-core`.
- Keputusan desain: mock provider in-process (pola mock_comcu), format
  tool-call satu baris `TOOL:`/`FINAL:` (seam JSON di Q2), qabot = program
  one-shot dengan 3 skenario scripted (pola umon).

## 2026-09-29 11:00 — implementasi core

- `user/qabot/`: qabot.h, history.c (append-only), tools.c (registry 4 tool:
  get_info/get_time/gpio_read/gpio_write + klasifikasi risiko), policy.c
  (gate ALLOW/CONFIRM/BLOCK, dicek SEBELUM eksekusi), provider.c (mock
  scripted + seam Q2 terdokumentasi), loop.c (ReAct + guardrail
  MAX_STEPS=8/MAX_MS=30000), eventlog.c (snprintf mini %s/%u/%d/%c),
  main.c (3 skenario scripted, verifikasi via qb_ev_contains).
- Wire: build-md.sh §1e (compile 7 .c -> link -> embed 32768), user.c
  uprogs[] += qabot (one-shot, pola umon). Branch `qabot/q1-harness-core`.

## 2026-09-29 11:10 — debug boot (2 bug)

1. `_start` hilang: tiap program define `_start` sendiri (section
   `.text.start`); main.c cuma punya `main` -> entry 0x0. Fix: tambah
   `_start` -> `main()`.
2. Stack overflow: `struct qb_run` ~13KB di stack, stack user cuma 1 page
   (4KB) -> exit 0xdab7. Fix: `static` (BSS).
3. S2 FAIL: pin=40 >= GPIO_PINS_PER_BANK(32) -> gpio_set -1. Fix: pakai
   pin=5 (juga diupdate di PLAN.md).

## 2026-09-29 11:15 — verifikasi QEMU

- `QABOT: 3/3 PASS` (S1 get_info/ALLOW, S2 gpio_write/CONFIRM+auto-yes,
  S3 self_destruct/BLOCK tanpa eksekusi). Event log urut + timestamp.
- `user_launch_init: PASS (9/9 programs, 17/17 files)`, 0 FAIL di log,
  daemon face/uiapp/ntp terdaftar normal.
