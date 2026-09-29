# Riset: Agent Runtime Harness
_Tanggal: 2026-09-28. Konteks: persiapan agent loop on-device untuk QaonicOS (RV1103). QaonicOS masih on-dev, jadi dokumen ini murni riset harness — bukan desain fase._

## 1. Definisi: harness vs model

> "An agent harness is the runtime around a model that decides what the model sees, what actions it can take, how actions execute, how work is recorded, and how the system recovers, stops, and is evaluated."
> — tenzki/forage, `docs/agent-harness-research.md`

Model hanya melihat teks — ia tidak bisa mengeksekusi apa pun sendiri. Harness menjembatani output teks model menjadi operasi sistem nyata, lalu mengembalikan hasilnya. Konsekuensi praktisnya: **model yang sama dicolok ke dua harness berbeda = dua agent yang perilakunya beda total** (linkedin.com, "Agent Harness vs Platform Harness"). Jadi untuk QaonicOS, yang perlu dibangun bukan "AI"-nya, melainkan harness-nya; otaknya bisa API provider.

## 2. Anatomi harness generik

Dari sintesis beberapa sumber, harness lengkap memiliki komponen ini:

| Komponen | Tugas |
|---|---|
| Input | Manajemen sesi, trigger (user/teks/sensor), steering |
| Context / Memory | System prompt, riwayat percakapan (canonical history), skill, kompresi konteks |
| Model runtime | Pemilihan provider/model, streaming, retry, abort, pelacakan usage/biaya |
| Tools | Skema (JSON schema), validasi argumen, eksekusi, otorisasi, error hasil |
| Agent loop | model call → tool calls → eksekusi → hasil → model call … → jawaban final |
| Policy / Governance | Izin kapabilitas, rate-limit, gate konfirmasi untuk aksi destruktif |
| Observability | Event log, trace, audit — apa yang diputuskan dan dieksekusi |
| Durability | Recovery saat interupsi, idempotensi, outcome terminal |

Bentuk minimalnya kecil saja (adarsha.dev, "Inside a coding agent harness"):

```
messages:  riwayat pesan = satu-satunya state
model:     handle provider-agnostic
tools:     ToolSet (registry)
policy:    PermissionPolicy
systemPrompt, usage tracker
```

## 3. Loop inti (ReAct)

Pola universalnya — think → act → observe → adjust:

```
loop:
  prompt = compose(system_prompt, history, tool_schemas)
  response = model.call(prompt)            # streaming, dengan timeout/abort
  if response.tool_calls kosong:
      history.append(assistant_turn); return final_answer
  for tc in response.tool_calls:
      if not policy.allow(tc): -> block / minta konfirmasi / eskalasi
      result = tools.execute(tc)           # dengan timeout & error capture
      history.append(tool_result)
  guard: max_steps / max_waktu / max_biaya tercapai? -> terminate
```

Varian `agent-harness-skeleton` (TS) menambahkan gate eksplisit per ronde: `shellCall → gate(state) → streamChat → metrics → runGuards → trip? terminate`, dan tool dieksekusi lewat registry dengan "doors" (pintu izin) di dalamnya. Intinya: **policy check terjadi SEBELUM eksekusi, bukan sesudah**.

## 4. Studi kasus

### 4a. tiny-agent-runtime — referensi arsitektur paling relevan
https://github.com/juntoku9/tiny-agent-runtime — Rust `no_std`, otak LLM di cloud, chip IoT sebagai tool. Struktur crate-nya patut ditiru:

```
tar-core    no_std · agent loop, tools, kontrak memory (ZERO I/O)
tar-hal     trait: Storage, Clock, HttpClient, Transport, Actuator, Camera
tar-proto   protokol node + skema tool, dipakai semua node
tar-llm     provider Anthropic (fallback OpenAI-compatible)
tar-tools   built-in tools (gpio, …), feature-gated untuk build kecil
tar-policy  capability + rate-limit + human-confirm engine
platforms/  tar-linux, tar-esp32  ← satu-satunya tempat I/O berada
```

Prinsip kuncinya: **"The brain is pure logic; platform crates are thin and are the only place I/O lives."** Ini peta 1:1 ke QaonicOS — core harness sebagai program userspace murni logika, semua I/O lewat syscall (di QaonicOS, "HAL trait" = nomor syscall). Mereka juga memisahkan node tool remote via protokol LAN dengan dynamic discovery — relevan kalau nanti ada multi-board.

Status: early development, policy enforcement + ESP32 hardware masih ⏳.

### 4b. MimiModel — bukti harness C mungil bisa jalan di MCU
https://github.com/memovai/mimimodel — engine inferensi + tool-calling **satu file C99 (~2000 baris)**, LLM 45M parameter jalan full offline di ESP32-S3, bobot di flash (memory-mapped). Pelajaran: harness tool-calling tidak butuh runtime besar; satu file C + `libm` cukup untuk loop "minta tool call → eksekusi GPIO → kembalikan hasil".

### 4c. smolagents (HuggingFace) — varian "aksi sebagai kode"
CodeAgent menulis aksi sebagai snippet Python, bukan JSON tool-call — terbukti **30% lebih sedikit step** (lebih sedikit panggilan LLM) dan performa lebih tinggi di benchmark sulit. Trade-off untuk embedded: butuh interpreter di device. Untuk QaonicOS (tanpa Python), JSON tool-calling tetap pilihan wajar; pola code-action bisa dipertimbangkan jauh nanti via interpreter mungil (mis. Lua).

### 4d. Pola gateway — ESP32 Agent Dev Kit / ESP-Claw
Harness di device, LLM 100% di cloud/server. Ini pola yang paling murah untuk v1: device tidak butuh inferensi lokal sama sekali, hanya butuh HTTPS client + loop.

## 5. System-1 di dalam harness (tempat Jev-like duduk)

Harness modern memisahkan dua kecepatan keputusan:

- **System-2 (lambat, mahal):** panggilan LLM cloud untuk reasoning & planning.
- **System-1 (cepat, murah):** keputusan refleks di dalam harness, dipanggil di titik-titik sempit:
  - _Pre-tool gate:_ `noul("aksi ini destruktif?")` sebelum eksekusi tool berisiko.
  - _Routing:_ `choice("tanya LLM / jawab dari cache / tolak")`.
  - _Verifikasi:_ `score("hasil tool sesuai ekspektasi?")` sesudah eksekusi.

Kontraknya (sesuai pola Jev): selalu baca `confidence` sebagai sumbu kedua — confidence rendah → eskalasi ke System-2, jangan bertindak. Jev sendiri cloud-only; Laya (421M, Apache 2.0) open-source tapi butuh ~1GB RAM — kegedean untuk RV1103 (64MB). Kandidat on-device realistis: classifier TinyBERT-scale (~15MB int8) yang di-fine-tune untuk primitif choice/score/noul, atau mulai dari rule-based + keyword untuk gate destruktif.

## 6. Kebutuhan minimal v1 (agar tidak over-engineering)

Dari riset di atas, v1 harness cukup punya:

1. Message history sebagai state kanonis (append-only).
2. Satu model provider (OpenAI-compatible HTTPS) + retry/timeout/abort.
3. Tool registry: nama, JSON schema, fungsi eksekusi, klasifikasi risiko (aman/konfirmasi/blokir).
4. Policy gate sebelum setiap eksekusi tool.
5. Guardrail loop: max steps, max waktu, max biaya.
6. Event log minimal (observability): timestamp, keputusan, tool, hasil.
7. **Jangan** di v1: multi-agent, MCP, RAG/vector DB, kompresi konteks otomatis — tambah saat ada kebutuhan terukur.

## 7. Pemetaan ke QaonicOS (nanti, saat difasekan)

- Harness = **program userspace** (seperti `umon`), bukan kernel. Core logika murni; I/O via syscall.
- Tools v1 bisa langsung menempel ke syscall yang sudah ada: GPIO (40/41), SD (50/51), FAT32 (52–56), STAT (57).
- Yang belum ada dan menjadi prasyarat: **TCP client + TLS** (mbedTLS di userspace, ~200–400KB) untuk memanggil API provider; driver audio input untuk STT (fase terpisah).
- Policy engine paling natural duduk **di antara harness dan syscall destruktif** (mis. FAT_DELETE, SD_WRITE) — cerminan pola "doors" di agent-harness-skeleton.
- Budget RAM 64MB: harness C + mbedTLS + buffer JSON (KB) sangat muat; yang ketat adalah jika STT/decision model ikut on-device.

## 8. Pertanyaan desain yang masih terbuka

1. JSON tool-calling vs code-action (butuh interpreter) — untuk v1: JSON.
2. Protokol tool: skema sendiri vs MCP client mungil — MCP menambah interop, tapi biaya parsing.
3. Di mana System-1 on-device duduk: thread terpisah via Mach IPC, atau fungsi dalam proses harness?
4. Model provider default: OpenRouter (sudah ada jalur kredensial) vs langsung ke vendor.

## Sumber utama
- https://github.com/juntoku9/tiny-agent-runtime (arsitektur crate + ARCHITECTURE.md)
- https://github.com/tenzki/forage/blob/HEAD/docs/agent-harness-research.md (definisi & komponen)
- https://github.com/adarshaacharya/adarsha.dev/blob/HEAD/src/content/inside-coding-agent-harness.mdx (harness minimal)
- https://github.com/uos1231234/agent-harness-skeleton/blob/HEAD/docs/ARCHITECTURE.md (runIMLoop + guards)
- https://github.com/memovai/mimimodel (tool-calling C99 di ESP32-S3)
- https://huggingface.co/docs/smolagents (CodeAgent: aksi sebagai kode, −30% steps)
