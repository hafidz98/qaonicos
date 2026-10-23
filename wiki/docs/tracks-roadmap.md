# Tracks & Roadmap

Work is organised in tracks of numbered phases. Each phase ships
with implementation + QEMU verification + a green regression
battery + a dev log + a commit. The running order below was
locked by the owner on 2026-10-23.

> **Key facts**
>
> - **Locked order: FND-1..5 → C6L-1..5 → UI-1..6.**
> - FND-1 and FND-2 are **done**; **FND-3 is in progress**.
> - SOC, PWR and THR are fully researched but **proposed, not scheduled** — they slot in only by owner decision.
> - Everything in the hardware-gated list waits for the physical board (on the owner's savings clock — it is not chased).

## Foundations track (FND) — running

Scope: the top 5 of the 14-item board-less foundation audit
([`docs/riset-fondasi-tanpa-board/RINGKASAN.md`](../docs/riset-fondasi-tanpa-board/RINGKASAN.md));
items 6–14 are the track's explicit backlog (factory reset first).
Plan: [`docs/rencana-fondasi-tanpa-board/PLAN.md`](../docs/rencana-fondasi-tanpa-board/PLAN.md).

| Phase | Content | Status |
|---|---|---|
| FND-1 | Reset/poweroff syscalls 106/107 via PSCI | ✅ done — PASS 16/16 |
| FND-2 | Resource reclamation on daemon death | ✅ done — test-first battery (fail pre-fix, pass post-fix) |
| FND-3 | Watchdog / health supervisor (+ HW WDT arming on hardware) | 🔄 in progress |
| FND-4 | Persistent littlefs backend in QEMU | pending |
| FND-5 | Update delivery path (`update stagepkg <path>`) | pending |

Details: [Boot & Resilience](boot-resilience.md),
[Storage](storage.md), [Updates](updates.md).

## C6 link track (C6L) — next

| Phase | Content |
|---|---|
| C6L-1 | Link-layer formalisation + wire freeze |
| C6L-2 | Host-bridge backend + Python mock C6 peer |
| C6L-3 | v2 completion: TOOL.DISPATCH, EVT, NET.WS framing |
| C6L-4 | AUD streaming integrity at simulated 921600 baud |
| C6L-5 | linkd — single transport owner daemon |

Details: [Networking & the C6 Link](networking-c6.md).

## UI track (UI) — after C6L

LVGL inside uiapp only; UI-1 is a measure-first go/no-go against
the ~287 KB boot-partition headroom. UI-2 shell/status/menu proof,
UI-3 Settings subtree, UI-4 Monitor + Power, UI-5 text edit +
polish, UI-6 cleanup + size audit + full battery.
Details: [Display, UI & Face](display-ui-face.md).

## Proposed tracks (researched, unscheduled)

| Track | Phases | One-line scope |
|---|---|---|
| SOC | SOC-1..6 | Exploit the RV1103: platform plumbing (CRU/pinmux) → entropy + crypto → storage/USB gadget → RGA2 → vision (product gate) → bench & small wins. Details: [Hardware & SoC](hardware-soc.md) |
| PWR | PWR-1..4 | Power model + simulated source (board-less) → SARADC → real PMU sleep → battery productisation. Details: [Power & Battery](power-battery.md) |
| THR | THR-1..5 | FPU/NEON context (top adopt item) → cooperative hardening → quantum preemption *(gated on a measured jitter need)* → HPMCU ground truth (hardware) → C6 JOB protocol |

## Hardware-gated list

Waits for the Luckfox Pico Mini B (and/or the ESP32-C6) itself:
B0.4 flash test; the real NAND driver (B1-HW) and SPI/ST7789
bring-up; ONB-3/4/5 (BLE + SoftAP provisioning transports);
OTP-wrapped credential key; persisted PIN-throttle counters;
the H1–H4 hardware test plan (Pico alone → Pico+C6 → C6 as Muse
Gadget → audio); C6 firmware FW-1..3; HPMCU ground truth (THR-4);
PWR-2..4.

## Parked by owner decision (do not start, do not re-propose)

| Item | Since |
|---|---|
| Secure boot / OTP fusing (B5.4) | 2026-10-21 — deferred until fully understood |
| LLM API key + Mode A live test | 2026-10-21 — "skip for now"; offline design covers it |
| Multi-user support (D-ONB-7) | 2026-10-21 — v1 is one owner PIN |
| QEMU CI on the working branch | backlog #11 — CI currently triggers on `core` only |

## Open decisions waiting on the owner

- **D-ARCH-1..4** — HTTP server's fate; R14 adoption; netstack's
  long-term home; IPC build-or-bury ([boundary page](kernel-userspace-boundary.md)).
- **Q-A1..Q-A7** — the architecture research's open questions
  (daemon image delivery, the network pump's owner, the Mode-B
  serving surface, one written secrets rule, …) in
  [`docs/riset-arsitektur-os/RINGKASAN.md`](../docs/riset-arsitektur-os/RINGKASAN.md) §4.
- Scheduling of **PWR-1** and **THR-1** (both board-less, both
  designed) and the **SOC** track.
- GitHub Wiki/Projects activation (one manual click; a draft
  exists outside the repo).

Sources:
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§8),
[`docs/rencana-fondasi-tanpa-board/PLAN.md`](../docs/rencana-fondasi-tanpa-board/PLAN.md),
[`docs/rencana-komunikasi-c6/PLAN.md`](../docs/rencana-komunikasi-c6/PLAN.md),
[`docs/riset-ui-framework/RINGKASAN.md`](../docs/riset-ui-framework/RINGKASAN.md),
[`docs/riset-soc-rv1103/RINGKASAN.md`](../docs/riset-soc-rv1103/RINGKASAN.md) (§4).
