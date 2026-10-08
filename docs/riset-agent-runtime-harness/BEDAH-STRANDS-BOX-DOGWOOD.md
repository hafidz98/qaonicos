# Bedah: Strands Box + Dogwood → Spec Policy Gate Qabot
_Tanggal: 2026-10-08. Sumber: AWS Open Source Blog "Introducing Strands Box"
(2026-10-06) + The Dogwood Guide (dogwood-policy.github.io)._
_Tujuan: mengadopsi model policy Strands Box/Dogwood ke policy gate Qabot
(`policy.c`, Q1) dalam bentuk yang muat di RV1103 (64MB RAM)._

## 1. Ringkasan Strands Box

Strands Box (developer preview, Box-nya Apache 2.0) adalah sandbox untuk
menjalankan AI agent: **containment OS-level + policy engine**. Containment
(macOS Seatbelt; Linux masih roadmap) memberi batas keras apa yang bisa
dijangkau agent. Di dalam batas itu, **policy** mengatur aksi apa yang boleh
dilakukan — dan policy inilah yang relevan untuk Qabot.

**Titik enforcement** (semua di luar proses agent):
| Titik | Yang diawasi |
|---|---|
| Egress gateway (proxy) | semua traffic keluar: host, port, method, path |
| Shell interpreter | perintah shell → event `fs:read`, `fs:delete`, `shell:spawn`, … |
| Python interpreter | operasi file/jaringan dari kode Python |
| MCP broker | pemanggilan MCP tools |

Kuncinya: **kosakata event seragam, agnostik terhadap tool**. Baca file lewat
shell maupun lewat Python sama-sama jadi event `fs:read`; HTTP dari curl maupun
Python sama-sama `http:request`. Aturan policy ditulis terhadap event, bukan
terhadap tool spesifik.

Dua mekanisme penting:
- **Penolakan yang bisa dibaca agent**: aksi yang ditolak dikembalikan sebagai
  HTTP 403 berisi `policy denied`, nama rule (`@id`), dan deskripsinya
  (`@description`) — agent bisa adaptasi (mis. menunggu, bukan retry membabi buta).
- **Injeksi kredensial di gateway**: agent hanya memegang token placeholder;
  gateway menukar dengan secret asli saat request keluar. Secret tidak pernah
  masuk ke environment agent. (Metode: Bearer, custom header, Basic, query
  param, AWS SigV4.)

Trade-off yang mereka akui: interpreter berjalan **di luar** sandbox (karena
mereka yang menegakkan policy) — trusted computing base jadi lebih lebar.

## 2. Bedah bahasa Dogwood

Dogwood adalah bahasa policy yang di-**lowering** ke Cedar + monitor stateful
(untuk bagian temporal). Struktur rule:

```
@id("nama_rule")
@description("penjelasan untuk agent")
permit|forbid (principal, action == Namespace::"event", resource)
when { <ekspresi Cedar atas context.input / context.output> }
unless { ... };
```

Konsep inti:
- **Scope** `(principal, action, resource)` — di Box, principal selalu agent dan
  resource fixed; yang di-match adalah **action** (nama event).
- **`context.input` / `context.output`**: detail event (mis. `context.input.host`,
  `context.input.method`, `context.input.program_path`). Output dipakai untuk
  keputusan berbasis hasil (mis. hanya hitung request yang return 200).
- **`when` / `unless` boleh banyak**, implisit di-AND-kan.
- **Temporal sublanguage** (`when temporal { ... }`): hanya masa lalu
  (past-only), window waktu wajib. Operator: `formerly within 10m`, `previous`,
  `since`, `exists`, agregasi `count`/`sum`, `tp` (timepoint). Contoh bentuk:
  "tolak jika `count` event X dengan response 200 dalam 10 menit terakhir >= 3".
- **Information providers**: pemanggilan `Ns::Fn(args)` (mis. filter konten,
  cek akses) — dikenali secara struktural, dievaluasi on-demand, hasilnya bisa
  dikomposisikan dalam ekspresi `when`. Diimplementasikan sebagai skrip Rhai.
- **Macros**: `def cedar` / `def temporal` untuk aturan reusable.
- **Event schema** (`.dwschema`): deklarasi event request/response + field-nya.
- **Toolchain**: CLI `dogwood` (`validate`, `replay`, `lower`, `check-parse`) +
  Rust API (`PolicySchema → LoweredPolicySet → Validator → Authorizer`).
  Setiap contoh di guide adalah bundle runnable yang di-test otomatis.

## 3. Perbandingan: Dogwood vs policy gate Qabot (Q1)

| Aspek | Dogwood / Strands Box | Qabot Q1 (`policy.c`) |
|---|---|---|
| Verdict | biner: `permit` / `forbid` | 3-state: ALLOW / CONFIRM / BLOCK |
| Yang di-match | event (`fs:read`, `http:request`, …) | nama tool + args |
| Kondisi | ekspresi Cedar + temporal + providers | klasifikasi risiko statis per tool |
| History | event log sebagai state monitor | event log ada, belum dipakai untuk keputusan |
| Rate limit / guardrail | aturan temporal deklaratif | hardcoded (`MAX_STEPS=8`, `MAX_MS=30000`) |
| Alasan penolakan | `@id` + `@description` dikembalikan ke agent | reason string, tercatat di event log |
| Bahasa/implementasi | Rust + Cedar (berat) | C, tabel statis (ringan) |
| Enforcement | di luar proses agent (proxy/interpreter) | di dalam harness, sebelum `tools.execute` |

## 4. Yang diadopsi untuk Qabot

1. **Kosakata event seragam.** Qabot v1 hanya punya tool-call; saat tool
   bertambah (network, file), definisikan event `Qabot::Action::*`:
   `tool:call`, `tool:result`, `net:request`, `fs:write`. Aturan ditulis
   terhadap event — tool baru otomatis tercakup.
2. **Alasan terstruktur.** Ganti reason string bebas dengan pasangan
   `(rule_id, description)` ala `@id`/`@description`. Event log mencatat
   `rule_id`; description dikembalikan ke model agar bisa adaptasi
   (paralel dengan 403 Box).
3. **Guardrail sebagai aturan temporal, bukan konstanta.** `MAX_STEPS=8` bisa
   ditulis sebagai rule "forbid `tool:call` jika count `tool:call` dalam task
   ini >= 8" — satu mekanisme untuk semua batas (steps, waktu, biaya, rate).
4. **Pola injeksi kredensial.** Saat TLS client jadi (prasyarat LLM provider),
   API key JANGAN disimpan di environment harness — simpan di luar loop agent
   dan tempel saat request keluar (di Qabot: di lapisan transport, bukan di
   `loop.c`).
5. **Penolakan yang informatif ke model.** Saat ini BLOCK hanya tercatat;
   teruskan `rule_id` + `description` sebagai `tool_result` khusus agar model
   tahu ia ditolak *aturan*, bukan error tool.

## 5. Yang TIDAK diadopsi / disederhanakan (alasan: 64MB RAM)

- **Engine Cedar/Rust**: terlalu berat untuk RV1103. Qabot pakai **subset**:
  tabel rule statis di C, tanpa parser di device. Policy ditulis off-device
  (di repo, gaya Dogwood sebagai *spec*), lalu di-compile jadi tabel C
  (atau struct const) saat build.
- **Information providers (Rhai)**: tidak ada di v1. Kebutuhan provider
  (mis. "apakah aksi ini destruktif?" ala System-1) dipenuhi rule-based dulu,
  sesuai riset harness §5.
- **Macros**: tidak perlu di device; macro hanya dipakai off-device saat
  menulis spec (kalau perlu).
- **Containment OS-level**: di luar scope Qabot — itu kerjaan kernel/QaonicOS,
  bukan harness. Yang Qabot tiru hanya lapisan *policy*-nya.
- **Verdict 3-state dipertahankan.** Dogwood biner (permit/forbid); Qabot butuh
  CONFIRM (konfirmasi fisik di device nyata, pola §12 RENCANA app). Pemetaan:
  `permit` → ALLOW, `forbid` → BLOCK, dan CONFIRM dimodelkan sebagai
  `permit` + *obligation* `confirm:physical` — di device uji jadi auto-yes
  tercatat, sesuai perilaku Q1 sekarang.

## 6. Draft spec: aturan policy Qabot gaya Dogwood

> Notasi di bawah adalah **spec off-device** (gaya Dogwood, disederhanakan).
> Implementasi device = tabel C ekuivalen di `policy.c`. `principal` selalu
> agent; `resource` fixed (device).

```
// ---- Tools baca-only: selalu boleh ----
@id("allow_readonly_tools")
@description("Read-only tools need no confirmation")
permit (principal, action == Qabot::Action::"tool:call", resource)
when {
  context.input.tool_name == "get_info" ||
  context.input.tool_name == "get_time" ||
  context.input.tool_name == "gpio_read"
};

// ---- Tulis GPIO: perlu konfirmasi fisik ----
@id("confirm_gpio_write")
@description("GPIO writes need physical confirmation")
permit (principal, action == Qabot::Action::"tool:call", resource)
when { context.input.tool_name == "gpio_write" }
obligation { confirm:physical };

// ---- Rate limit: cegah relay chatter ----
@id("rate_limit_gpio_write")
@description("Max 10 GPIO writes per minute")
forbid (principal, action == Qabot::Action::"tool:call", resource)
when { context.input.tool_name == "gpio_write" }
when temporal {
  (count for (t: Timepoint). where (
    formerly within 1m (
      Qabot::Action::"tool:call"::request{ input.tool_name: "gpio_write" }
      && tp(t)
    )
  )) >= 10
};

// ---- Guardrail steps sebagai aturan, bukan konstanta ----
@id("guard_max_steps")
@description("Task ends after 8 tool calls")
forbid (principal, action == Qabot::Action::"tool:call", resource)
when temporal {
  (count for (t: Timepoint). where (
    formerly within 30m (
      Qabot::Action::"tool:call"::request{} && tp(t)
    )
  )) >= 8
};

// ---- Default deny: tool tak dikenal selalu diblokir ----
@id("deny_unknown_tool")
@description("Unknown tools are never executed")
forbid (principal, action == Qabot::Action::"tool:call", resource)
when { !(context.input.tool_name in ["get_info", "get_time",
        "gpio_read", "gpio_write", "self_destruct"]) };

// ---- Tool uji destruktif: blokir eksplisit (menggantikan self_destruct) ----
@id("deny_destructive_test")
@description("Destructive test tool is blocked")
forbid (principal, action == Qabot::Action::"tool:call", resource)
when { context.input.tool_name == "self_destruct" };
```

Urutan evaluasi (ditentukan, bukan acak): `forbid` dievaluasi dulu
(default-deny aman); `permit` dengan obligation → CONFIRM; `permit` polos →
ALLOW. Ini mempertahankan semantik Q1 (`policy_check` → ALLOW/CONFIRM/BLOCK).

## 7. Rencana implementasi ringan (di device)

1. **Spec tinggal di repo** (`docs/riset-agent-runtime-harness/`, file ini)
   sebagai sumber kebenaran aturan.
2. **`policy.c` v2**: ganti if/else klasifikasi dengan tabel:
   ```c
   typedef struct { const char *tool; verdict_t base; const char *rule_id;
                    const char *desc; int max_per_min; } policy_rule_t;
   static const policy_rule_t POLICY[] = { ... }; // di-compile dari spec
   ```
3. **State temporal**: ring buffer timestamp per tool (fixed-size, mis. 16
   slot) + counter steps per task — O(1) amortized, tanpa alokasi dinamis.
4. **Event log** sudah ada — jadikan sumber `formerly` (cukup untuk window
   menit; tidak perlu query kompleks).
5. **Obligation `confirm:physical`**: di QEMU = auto-yes tercatat (seperti
   sekarang); di device = tombol fisik (§12 RENCANA app).

## 8. Pertanyaan terbuka

1. Saat tool network/file bertambah, apakah event vocabulary
   `Qabot::Action::*` cukup, atau perlu namespace per subsistem?
2. Confirm fisik: apakah ada aksi yang butuh *dua* tombol / long-press
   (pola konfirmasi berlapis)?
3. Apakah `policy.dw`-asli (Dogwood beneran) layak dipakai **off-device**
   untuk *validasi* spec via `dogwood validate`/`replay` — spec ditulis
   sekali, di-test dengan trace, lalu di-compile ke tabel C? (Rekomendasi:
   ya, saat tool > 10.)
4. Credential injection: di mana tepatnya secret store duduk di QaonicOS —
   userspace daemon terpisah atau di dalam `qabotd`?

## Sumber
- https://aws.amazon.com/blogs/opensource/introducing-strands-box-ai-agent-sandboxes-powered-by-dogwood
- https://dogwood-policy.github.io/dogwood/ (The Dogwood Guide)
- https://github.com/strands-agents/box
- `docs/riset-agent-runtime-harness/RINGKASAN.md` (§2–§7)
- `docs/rencana-qabot-harness/PLAN.md` (§4–§6: tools v1, policy gate, guardrail)
