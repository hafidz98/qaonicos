# Power & Battery

Today's "sleep" is a **display state, not a power state**: the
panel goes black and the face stops rendering, but the SoC stays
fully clocked and no code touches the PMU. This page holds the
research verdict and the PWR track that turns power into a real
subsystem — sensing first, policy second, true SoC sleep last.

> **Key facts**
>
> - `qaon_stat` has **no battery field at all** (its UNKNOWN fields are `cpu_pct` and `blk_used_sec`). Battery support is a design, not a missing readout.
> - The RV1103 datasheet publishes **no SoC current-consumption figures** — every budget number is a labelled estimate until the board is measured (PWR-3 makes the measurement a mandatory exit artifact).
> - **Battery sensing:** SARADC **IN1** (ball 15, header pos 8) behind a 200k/100k divider. IN0 is the board's RECOVERY key — not available.
> - **Wake sources are domain-bound:** only VD_PMU-domain sources (GPIO0 interrupt among them) wake the deep states — the button and the C6 wake line must land there.
> - The proposed ABI is one syscall, **`SYS_POWER` (108)**, shaped so a gauge IC can replace the voltage model later without an ABI change.

## The sleep-state ladder (RV1103, TRM terms)

Normal → SCU low power → Bus low power (DDR self-refresh) → DDR
low power (retention) → power-gating → clock-and-reset → **Sleep**,
plus a VD_PMU-only power-off where wake means reset. Entry:
configure, set `PMU_PWR_CON[0]`, CPU executes WFI; the PMU FSM
sequences clock gating / PLL shutdown / DDR self-refresh /
optional VD_LOGIC gating. Wake sources (`PMU_WAKEUP_INT_CON`):
CPU interrupt, GPIO0 interrupt, SDMMC detect, SDIO, USB detect,
HPTIMER, PMU timeout, software.

Two open questions decide how big the real-sleep work is:

1. **PMU identity** — RV1103 vs RV1106 TRM; to be confirmed via
   `PMU_VERSION` (`0x030A0000`) on hardware.
2. **Does production sleep need LPMCU firmware** (TRM §7.6.2), or
   is the hardware-FSM path (§7.6.1) sufficient? No PM firmware
   blob for the RV1103B exists in the local rkbin — hinting at
   FSM-only — but this is the difference between days and weeks
   for PWR-3. (The SoC/threading research adds a taxonomy fix:
   the **LPMCU** (PMU sleep sequencer, VD_PMU) and the **HPMCU**
   (camera fast-boot core, VD_CORE) are two different RISC-V
   blocks.)

Also noted: the RTC sits in VD_LOGIC per the TRM, so timed wake
from deep states may have to fall back to HPTIMER / PMU timeout.

## The OS design (PWR-1, board-less half)

- **Sampling in kernel, policy in the face.** The SARADC driver is
  a tiny shared polled peripheral (the audio track set the
  precedent). Sampling is on-read with a 1 s kernel cache — one
  conversion is ~200 µs and nothing samples in the background, so
  gauging never wakes a sleep state. The **face owns policy**: it
  already owns the power FSM and the display token, polls
  `SYS_POWER` at 1 Hz while awake, and acts on the ladder.
  qabotd/uiapp only *read* for display.
- **Percentage from voltage, honestly.** v1 is a filtered
  resting-voltage model with quiet-moment commits and threshold
  hysteresis — not a coulomb counter. The `flags` field marks the
  source (0 = SARADC model, 1 = gauge IC) so a MAX17048/CW2015-class
  gauge can take over later.
- **Low-battery ladder** (on the committed percentage, 2%
  hysteresis each, every step bootlogged):

    | Level | Action |
    |---|---|
    | ≥ 25% | normal |
    | 20% | status-bar battery turns warning colour; Monitor shows LOW |
    | 15% | full-screen warning; brightness step down (or shorter display-on timeout until backlight control exists) |
    | 10% | "dying-gasp" preparation: flush all state, arm the `BLE_LOWBATT` bootlog note, notify the C6, warn continuously |
    | 5% | graceful shutdown: bootstate note (reason 4) → `SYS_POWEROFF` |

    The 10% pre-arm is the point: a cell that dies hard at 4%
    leaves no energy for NAND writes, so the intent must already
    be on disk — and a hard death must not be misread as a crash
    on the next boot.
- **In QEMU**, the kernel fills the struct from a **simulated
  source** (a scripted voltage curve the harness can drive), so
  the whole policy/UI stack is testable board-less — the standing
  PWR-1 pattern every hardware subsystem follows.

## The PWR track

| Phase | Content | Needs board? |
|---|---|---|
| PWR-1 | Power model + simulated source + policy ladder + UI | no |
| PWR-2 | SARADC driver + calibration | yes |
| PWR-3 | Real SoC sleep (the H1.4 power-FSM test executes here; a measured supply/current table is a mandatory exit artifact) | yes |
| PWR-4 | Battery productisation: charger/power-path part choice, gauge-IC decision, C6 sleep choreography + wake wire | yes |

PWR-1 is designed and board-less but **not yet scheduled** — it
awaits the owner's prioritisation alongside SOC and THR.

Sources:
[`docs/riset-power-battery/RINGKASAN.md`](../docs/riset-power-battery/RINGKASAN.md)
(§0 honest state, §1 PMU, §2 SARADC, §4 OS design),
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§5.7),
[`docs/ARSITEKTUR.md`](../docs/ARSITEKTUR.md) (§5, SYS_POWER placement).
