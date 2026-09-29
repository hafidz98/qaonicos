# PLAN — Qabot Harness Q1: core v1 (mock provider)

**Track:** Qabot (agent runtime harness mini untuk QaonicOS @ Luckfox)
**Tanggal:** 2026-09-29 · **Status:** implementasi berjalan
**Riset dasar:** `docs/riset-agent-runtime-harness/RINGKASAN.md` (§6 "Kebutuhan minimal v1")

## 1. Tujuan Q1

Harness v1 **jalan di QEMU sebagai program userspace**, teruji end-to-end
dengan **mock provider in-process** (pola `mock_comcu`). Tanpa TLS, tanpa
jaringan — seluruh loop ReAct + tool-calling + policy gate terbukti lewat
skenario scripted yang deterministik.

Yang **sengaja tidak** di Q1 (→ Q2): TCP client + TLS (mbedTLS), JSON parser
format OpenAI, provider real, STT audio, multi-agent, MCP, RAG.

## 2. Arsitektur file (`user/qabot/`)

```
user/qabot/
  qabot.h      — tipe inti: message, tool_call, tool_def, policy_verdict,
                 event; batas buffer (static, tanpa libc/malloc)
  history.c    — message history append-only (state kanonis)
  tools.c      — tool registry: nama, deskripsi/skema ringkas, fungsi
                 eksekusi, klasifikasi risiko
  policy.c     — policy gate: ALLOW / CONFIRM / BLOCK per tool (+ alasan)
  provider.c   — abstraksi provider: struct provider_ops { chat() };
                 implementasi MOCK (scripted) di Q1, seam untuk real di Q2
  loop.c       — agent loop ReAct: compose → chat → tool_calls →
                 policy gate → execute → observe → guard
  eventlog.c   — event log: timestamp (uptime), keputusan, tool, hasil
  main.c       — 3 skenario uji scripted → cetak PASS/FAIL → sys_exit
```

Prinsip tiny-agent-runtime: **core = logika murni, I/O hanya via syscall**
(di sini "HAL trait" = nomor syscall).

## 3. Protokol tool-call internal (Q1)

Format teks satu baris (gampang di-parse di bare-metal C, seam jelas ke JSON):

```
TOOL:<nama> <arg1>=<val1> <arg2>=<val2>
FINAL:<teks jawaban akhir>
```

Mock provider mengembalikan salah satunya per giliran. Parser Q1 hanya
mengerti dua bentuk ini; parser JSON format OpenAI datang di Q2 bersama
provider real (titik sambung didokumentasikan di `provider.c`).

## 4. Tools v1 (nempel ke syscall yang sudah ada)

| Tool | Syscall | Risiko | Keterangan |
|---|---|---|---|
| `get_info` | SYS_STAT (57) | AMAN | uptime, thread, memori |
| `get_time` | SYS_TIME_GET (70) | AMAN | jam dinding / 0 bila belum sinkron |
| `gpio_read` | SYS_GPIO_GET (40/41) | AMAN | baca pin |
| `gpio_write` | SYS_GPIO_SET (40/41) | KONFIRMASI | tulis pin — demo policy gate |
| `self_destruct` | — (tidak ada) | BLOKIR | tool fiktif untuk uji verdict BLOCK |

## 5. Policy gate

`policy_check(nama_tool, args)` → `{ALLOW, CONFIRM, BLOCK}` + alasan string.
- AMAN → ALLOW langsung.
- KONFIRMASI → di Q1: mode uji auto-confirm TAPI gate tetap dicatat di event
  log (`confirm:auto-yes`); di device nyata diganti konfirmasi fisik (pola §12
  RENCANA app: tombol fisik).
- BLOKIR / tool tak dikenal → BLOCK + alasan, tidak dieksekusi.

## 6. Guardrail loop

- `MAX_STEPS = 8` per tugas, `MAX_MS = 30000` (via `sys_uptime`).
- Terlaksana → terminate dengan status `GUARD_STEPS` / `GUARD_TIME`;
  tercatat di event log.

## 7. Skenario uji (scripted, deterministik)

| # | Prompt | Alur mock | Verifikasi |
|---|---|---|---|
| S1 | "berapa uptime?" | `TOOL:get_info` → hasil → `FINAL:` | history berisi tool_result; event log urut |
| S2 | "nyalakan gpio 40" | `TOOL:gpio_write pin=5 val=1` → gate CONFIRM (auto-yes, tercatat) → eksekusi → `FINAL:` | verdict CONFIRM muncul di log; pin tertulis |
| S3 | "hapus semua" | `TOOL:self_destruct` → gate BLOCK | tidak ada eksekusi; status BLOCK tercatat |

Kriteria lolos: ketiga skenario selesai dalam guard, urutan event log benar,
tidak ada tool BLOCK yang tereksekusi.

## 8. Kriteria selesai Q1

- [ ] `user/qabot/` lengkap, compile clang 18 bare-metal tanpa warning baru
- [ ] `build-md.sh` wire qabot sebagai program one-shot (seperti `umon`)
- [ ] Boot QEMU: `QABOT: S1 PASS / S2 PASS / S3 PASS`, 0 FAIL
- [ ] Event log tercetak rapi (timestamp, keputusan, tool, hasil)
- [ ] Commit branch `qabot/q1-harness-core`
- [ ] DEVLOG.md terisi per langkah

## 9. Risiko & catatan

- Bare-metal C tanpa JSON lib: format satu-baris Q1 adalah trade-off sadar;
  seam ke JSON didokumentasikan, bukan diimplementasi.
- `sys_gpio_set` di QEMU = mock (catatan Fase D) — verifikasi = "syscall
  terpanggil & gate tercatat", bukan efek fisik.
- Ukuran: target < 32KB bin (embed 32768 seperti program lain).
