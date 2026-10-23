# Research: Power Management + Battery (RV1103 / Luckfox Pico Mini B)

Date: 2026-10-23. Scope: RESEARCH ONLY — what the RV1103 and the
Pico Mini B actually offer for power management and battery
operation, what QaonicOS has today, and the OS-side design +
phases that follow. Companion docs: `docs/hw-addrs.md`,
`docs/rencana-uji-hardware/RENCANA.md` (H1.4),
`docs/rencana-komunikasi-c6/PLAN.md` (C6 link),
`docs/rencana-fondasi-tanpa-board/PLAN.md` (FND-1/FND-3).

Evidence levels used throughout:

- **[DS]** datasheet-documented (RV1103 Datasheet V1.4,
  2023-07-25, in `~/workspace/luckfox-pico-mini-b/documents/`)
- **[TRM]** RV1106 TRM V0.3 Part 1, ch. 7 (PMU). The RV1103
  shares the RV1106 PMU block (the project already inherits
  `rv1106.dtsi` for RV1103 wholesale — `docs/hw-addrs.md`);
  PMU identity on the real chip is open question Q1.
- **[SDK]** observed in shipping driver/SDK code or DTS
  (Luckfox SDK in `~/workspace/b0-sdk/luckfox-pico/`,
  `~/workspace/qaonic_os/hw/dts/`)
- **[EST]** estimate / community figure — labelled, never
  presented as measured fact.

## 0. Current state — the honest truth

**Today's "sleep" is a display state, not a power state.**
The power FSM lives in `user/face/main.c` (§11 header,
`UI_IDLE_DISPLAY_OFF_S` 120 s, `UI_SLEEP_AFTER_S` 300 s):

- At 120 s idle: `flush_black()` once (black pixels flushed
  through the normal display path) and rendering stops.
- At 300 s: **nothing further happens.** The code says so
  itself (`face/main.c`, at the `UI_SLEEP_AFTER_S` comment):
  "v1 = tetap display off (CPU idle via yield); sleep beneran
  (clock gating) diverifikasi di HW" — v1 stays display-off;
  real sleep (clock gating) is deferred to hardware.
- The main loop keeps running at full speed: it polls
  `sys_display_get_event()` and `sys_face_poll()` every
  iteration and ends with `sys_yield()`. In the whole
  production kernel, `wfi` exists only inside `halt_cpu()` /
  `machine_halt()` (`machdep.c`, `user.c`) — never as an
  idle path. No code anywhere touches the PMU registers
  (base `0xff300000`); there is no clock gating, no PLL
  shutdown, no DDR self-refresh, not even an ST7789
  sleep/display-off command or backlight control —
  `flush_black()` writes black pixels to a fully powered,
  fully lit panel.
- `SYS_DISPLAY_SLEEP` (68) is only a one-bit mailbox:
  uiapp sets it ("Sleep Now"), the face reads-and-clears it
  and does the same `flush_black()`. Wake = any input event,
  because the event queue never stopped being polled.

So the one-line truth: **"sleep v1" saves whatever the panel
would have drawn over black pixels, and nothing else; the
SoC burns full idle power indefinitely.**

**Battery is entirely absent.** `struct qaon_stat`
(`kernel/kernel/arm/user.c`, mirrored in `user/ulib/ulib.h`)
has **no battery field at all** (the fields marked
`QAON_UNKNOWN` are `cpu_pct` and `blk_used_sec`). The uiapp
status bar draws an empty battery outline whose comment
admits the reason (`user/uiapp/ui.c`): "tanpa ADC/Fuel
gauge = jujur kosong" — no ADC, no fuel gauge, honestly
empty. There is no battery readout anywhere in the UI.

**What FND-1 added** (2026-10-23): `SYS_RESET` (106) /
`SYS_POWEROFF` (107) via PSCI in QEMU; `kernel/kernel/arm/
psci.c` is the seam whose RV1103 mapping (PMU software
reset / watchdog fallback for reset, PMU power-down for
off) is a marked `TODO(HW)`. Bootstate already records
*why* a boot follows the previous one: `BSR_POWER` 1,
`BSR_CRASH` 2, `BSR_WATCHDOG` 3, `BSR_SOFTWARE` 4
(`bootstate.c:55-59`), with bootlog event 12 for the
software reset/poweroff note. The policy ladder in §4 is
designed on top of these, unchanged.

## 1. RV1103 power architecture

### 1.1 Voltage domains [TRM §7.3.1] [DS §1.2.3]

Three voltage domains, fed as separate rails on the board
(VDD_ARM / VDD_LOGIC / VDD_PMU):

| Domain | Contents (TRM Table 7-1, abridged) |
|---|---|
| VD_CORE | Cortex-A7, HPMCU (RISC-V), CRU_CORE, CORE_GRF/SGRF, mailbox, PVTM |
| VD_LOGIC | CRU + all PLLs (A/C/D/G), DDR controller (MSCH), and the peripheral ring: SARADC, TSADC, USB PHY, MAC PHY, ACODEC, CSI PHY, GPIO2, OTP, RTC, PAD_RING_TOP |
| VD_PMU | PMU itself, CRU_PMU, PMU_SRAM (8 KB), PMU_WDT, LPMCU, I2C1, HPTIMER, DDR fail-safe, GPIO0, PMUIO_IOC, NPOR, PAD_RING_PMU |

Two consequences matter for design:

- **GPIO0 and HPTIMER live in the always-on VD_PMU.** A
  wake button or a timed wake that must work in the
  deepest state has to be wired there. UART4 (the C6
  link) is in VD_LOGIC — it cannot wake the chip from a
  state where VD_LOGIC is off (see §4.5).
- **The RTC block is listed under VD_LOGIC**, not VD_PMU.
  The PMU's low-frequency clock source can be "PMU PVTM
  or RTC" [TRM §7.1], but whether RTC survives the
  deepest mode is not stated — timed wake may have to
  rely on the VD_PMU blocks (HPTIMER, PMU timeout
  counter). Open question Q3.

### 1.2 The PMU and its two operating modes [TRM §7.3.2, §7.6]

PMU base `0xff300000`, size `0x1000` [SDK: `rv1106.dtsi`
pmu node; also `docs/hw-addrs.md`]. Key registers:

| Register | Offset | Role |
|---|---|---|
| PMU_VERSION | 0x00 | version (reset value 0x030A0000) |
| PMU_PWR_CON | 0x04 | bit0 arms the low-power FSM |
| PMU_GLB_POWER_STS | 0x08 | global power state (below) |
| PMU_INT_MASK_CON | 0x0C | global interrupt mask during low-power |
| PMU_WAKEUP_INT_CON | 0x10 | wake source enables |
| PMU_WAKEUP_INT_ST | 0x14 | wake source status |
| PMU_PMIC_STABLE_CNT / OSC / PLL_LOCK | 0x24 / 0x28 / 0x2C | sequencing delays |
| PMU_WAKEUP_TIMEOUT_CNT | 0x48 | PMU timeout wake (reset 0x5DC0) |
| PMU_SCU_PWR_CON | — | bit2: bypass the WFI requirement |

`PMU_GLB_POWER_STS` state encoding [TRM §7.4]: `0x0`
normal, `0x1` SCU low power, `0x3` bus low power, `0x4`
DDR low power, `0x5` power-gating low power, `0x6`
clock-and-reset low power, `0x7` **sleep state**, `0x8`–
`0xd` the matching "active" (exit) phases. The PMU does
not expose Linux-style named states; the useful ladder
for QaonicOS, in TRM terms:

1. **Idle (WFI only)** — CPU parked, everything clocked.
   What `machine_halt` does today; not yet used as an
   idle path.
2. **Clock-gated idle** — CRU per-block clock gating
   [DS §1.2.3: "clock gating control for individual
   components"] + BIU idle requests (`PMU_BIU_IDLE_*`)
   under software (normal-mode) control.
3. **Low-power mode (FSM)** — the real sleep: set the
   configuration, set `PMU_PWR_CON[0]`, CPU executes WFI
   (or is bypassed via `PMU_SCU_PWR_CON[2]`), and the PMU
   FSM sequences the phases: idle requests to domains,
   auto clock switch to the low-frequency source, block
   clock auto-gating, PLL power-down, DDR into
   self-refresh (bus-low-power phase) or retention
   (core-clock phase), optionally VD_LOGIC power-gating
   with VD_PMU inputs clamped [TRM §7.1, §7.6.1].
4. **Power-off** — VD_LOGIC (and VD_CORE) off, VD_PMU
   alive; wake is a reset ("wakeup reset control in
   power off mode" [TRM §7.1]). Resume = cold boot;
   bootstate tells the next boot it was planned.

An alternative software mode [TRM §7.6.2] has the LPMCU
(the small RISC-V core in VD_PMU) execute the low-power
procedure by writing the software power-control
registers. Whether the production sequence needs LPMCU
firmware is open question Q2 — no PM/suspend firmware
blob for rv1103b was found in the local rkbin (only
hpmcu images), and §7.6.1 documents a pure-FSM path.

### 1.3 Wake sources [TRM §7.4, PMU_WAKEUP_INT_CON]

| Bit | Source | Design note |
|---|---|---|
| 0 | CPU interrupt | any unmasked GIC interrupt path |
| 1 | GPIO0 interrupt | **the button wake** (VD_PMU domain) |
| 2 | SDMMC detect | card insert |
| 3 | SDIO interrupt | — |
| 4 | USB device detect | plug-in wake |
| 5 | HPTIMER interrupt | timed wake (VD_PMU timer) |
| 6 | PMU timeout | `PMU_WAKEUP_TIMEOUT_CNT` expiry |
| 7 | Software wakeup | PMU-internal |

### 1.4 DDR behaviour [TRM §7.1, §7.6.3]

DDR (the in-package 64 MB DDR2) enters **self-refresh**
during the bus-low-power phase and **retention** during
the core-clock phase; contents survive both. Nothing in
the datasheet or TRM suggests a state that keeps DDR
contents with VD_LOGIC fully off other than these —
i.e., the deepest *resume-in-place* state is the FSM
sleep above; anything deeper is the power-off/reset
state. [Inference from the state list; flagged as such.]

### 1.5 DFS

[DS §1.2.3] the PMU modes save power "by different
frequency or automatic clock gating control or power
domain on/off control", and the CRU has 4 PLLs. But the
board's rails come from a fixed-output multi-rail
DC/DC (EA3036C, §3) — the OS cannot scale VDD_ARM on
this board, so **frequency scaling only, no DVFS**
[inference from the schematic rail structure]. The A7
at a lower PLL rate during idle-screen states is a
cheap PWR-3 experiment, not a foundation.

### 1.6 Current figures per state — there are none

The RV1103 datasheet contains **no SoC power-consumption
table**: the only currents in it are USB PHY drive
levels, codec mic bias, and the *audio codec's own*
consumption (standby 0.01 mA, record/playback 2.5 mA —
§3.8; do not mistake these for SoC figures). Every
per-state current for the RV1103 in this document is
therefore an estimate (§5) until H1.4 measures the real
board. Any document quoting a "datasheet sleep current"
for this chip is quoting something that does not exist.

## 2. SARADC — the battery-sense ADC

### 2.1 Identity and clocks [SDK]

| Item | Value | Source |
|---|---|---|
| Base | `0xff3c0000`, size `0x200` | `rv1106.dtsi` saradc node |
| IRQ | GIC_SPI 62, level-high | same |
| Clocks | `CLK_SARADC` + `PCLK_SARADC` (CRU) | same |
| ADC clock | 1 MHz (driver sets `clk_rate = 1000000`) | U-Boot driver (below) |
| Reference | board `vcc_1v8` rail (`vref-supply = <&vcc_1v8>`) | `rv1103-luckfox-pico-ipc.dtsi` |
| Channels | 2, single-ended, 10-bit | [DS §1.2.12] + driver data |

### 2.2 Electricals [DS §3.9, Table 3-12]

Resolution 10 bit, ENOB 9, DNL ±1 LSB, INL ±2 LSB,
VREFP typ 1.8 V, input capacitance 8 pF, sampling rate
up to 1 MS/s. Full scale = 1.8 V → **1 LSB ≈ 1.76 mV**
at the pin. (Not in `docs/hw-addrs.md` yet; add SARADC
when PWR-2 lands.)

### 2.3 Register map [SDK — shipping driver]

From the Luckfox SDK's U-Boot driver
`sysdrv/source/uboot/u-boot/drivers/adc/rockchip-saradc-v2.c`
(compatible `rockchip,rv1106-saradc`, data: 10 bits,
2 channels, 1 MHz). Offsets computed from its register
struct; the ones a polled driver needs:

| Register | Offset | Use |
|---|---|---|
| CONV_CON | 0x00 | write `(START\|SINGLE_MODE\|channel)` with the same value in bits 31:16 (write-enable mask); START=bit4, SINGLE_MODE=bit5 |
| T_PD_SOC | 0x04 | power-up delay; driver writes 0x20 |
| T_DAS_SOC | 0x0C | sample delay; driver writes 0x0C |
| END_INT_EN | 0x108 | bit0 = end-of-conversion IRQ enable (same hi-word mask rule) |
| END_INT_ST | 0x114 | write 1 to clear |
| DATA0 / DATA1 | 0x120 / 0x124 | result per channel, mask 0x3FF |

Conversion sequence (driver-observed): assert/deassert
the APB reset (`SRST_P_SARADC`), program T_PD_SOC /
T_DAS_SOC, enable END int, write CONV_CON, wait
(driver polls/IRQ with a 100 µs scale timeout; its own
`udelay(100)` after starting). Per-conversion cost is
thus **on the order of 100–200 µs and a few register
writes** — at a 1 Hz sampling cadence the ADC is
electrically free. The SDK's Linux example
(`~/workspace/luckfox-pico-mini-b/adc/adc.c`) goes
through IIO sysfs (`in_voltage{0,1}_raw`,
`in_voltage_scale` = 1800/1024 mV) and confirms both
channels are exposed to userspace on the stock image.

### 2.4 Which channel can sense a battery?

Datasheet pin list [DS §2.6]: **SARADC_IN0** = ball 16
(mux GPIO4_C0), **SARADC_IN1** = ball 15 (mux
PWM1_M1/GPIO4_C1). The board schematic (text
extraction, `schematic.pdf`) shows:

- IN0 is the **RECOVERY key** input, with the explicit
  note "SARADC_IN0 must always be pulled-up", and is
  routed to pin-header position 9 (PIH109).
- IN1 is routed to pin-header position 8 (PIH108),
  uncommitted.
- The SARADC pin domain is 1.8 V only (schematic sheet
  note "SARADC/PWM(1.8V only)") — consistent with the
  1.8 V reference.

**Recommendation: sense on IN1.** IN0 is reserved by
the board's recovery-key design and its mandatory
pull-up would fight a divider. A LiPo (3.0–4.2 V) needs
a divider into the 0–1.8 V window anyway: ratio ≥ 2.34
required; a 200 kΩ / 100 kΩ divider (3:1) maps
4.2 V → 1.40 V and 3.0 V → 1.00 V, draws 14 µA from
the cell, and gives ≈ 5.3 mV/LSB at the battery —
plenty against a ~±20 mV accuracy budget after
averaging. (Divider permanently attached; gate it with
a GPIO-driven FET only if 14 µA matters — it does not,
next to the SoC's mA-scale sleep floor.) Header
positions are from schematic text extraction and must
be confirmed against the physical board (Q5).

## 3. Board power path and the external battery

### 3.1 How the Mini B is powered today [schematic]

USB-C receptacle (5.1 kΩ CC pull-downs, device port) →
VBUS → the VSYS net (`VCC5V0_SYS`) → **EA3036C**
multi-rail DC/DC, which generates VDD_0V9 (CPU),
VCC_DDR, VCC_1V8, VCC1V8_CODEC and VCC_3V3. The pin
header exposes VBUS (PIH201) and a 3V3 output (PIH203)
per the schematic text. **There is no battery
connector, no charger IC, and no power-path circuitry
on the board** — verified by absence across the full
schematic text extraction (no charger/gauge part or
battery net anywhere). A battery therefore has to
inject a regulated 5 V at the VSYS/VBUS point (or at
the USB-C port) from an external module; the board
cannot tell the difference between that and a wall
adapter — which is exactly why "source: USB vs
battery" must be sensed explicitly (§4) if the OS is
to know.

### 3.2 Charger / power-path module classes

| Option | What it is | Power-path? | Cost class | Verdict |
|---|---|---|---|---|
| TP4056 module (+ separate boost) | 1 A linear charger, USB in, BAT out | **No** — load in parallel with the cell corrupts charge termination; the classic trap | ~$1 | Bench only; do not ship as the product path |
| Powerbank SoC module (IP5306 class) | charger + 5 V boost + LED gauge in one IC, I2C variant exists | Yes (it *is* a powerbank) | ~$2–4 | Simplest system story: board always sees "USB 5 V"; boost quiescent and always-on behaviour are the price; percentage from the host is coarse/none |
| Power-path charger IC (BQ24074 class) | charger with true power-path (system runs from input, cell charges independently) + separate 5 V boost for VSYS | Yes | ~$3–6 in parts | The correct product topology; most design work |
| Direct VSYS feed from cell + wide-input tolerance | — | — | — | **Rejected**: EA3036C input is a 5 V rail; a 3.0–4.2 V cell cannot feed it directly |

Whichever charger path is chosen, gauging is a
separate decision:

### 3.3 Sensing: SARADC divider vs gauge IC

| | SARADC IN1 + divider (§2.4) | MAX17048 (I2C) | CW2015 (I2C) |
|---|---|---|---|
| BOM add | 2 resistors (~$0) | ~$1.5–3 + breakout | ~$0.5 |
| What you get | raw cell mV only | VCELL (±7.5 mV typ) + ModelGauge SOC%, ALRT pin | VCELL + its own SOC% algorithm |
| SOC accuracy | ±5–10% typical after §3.4 mitigations | ±3% class, no learn cycles, no sense resistor | similar class; ubiquitous on Rockchip boards (upstream Linux driver exists) |
| Pins | 1 header pin (IN1) | I2C bus (2 pins) + optional ALRT | I2C bus (2 pins) + optional ALRT |
| Driver work | ~100 lines polled ADC (§2.3) | small register driver (VCELL/SOC regs) | small register driver |
| Quiescent | 14 µA divider (+ ADC on demand) | 23 µA active / 3 µA hibernate [MAX17048 datasheet] | µA class |
| Risk | load-sag errors are ours to mitigate | WLP/TDFN packages are hand-solder-unfriendly; breakout boards exist (Adafruit) | documentation thinner; algorithm is a black box |

**Recommendation: SARADC IN1 + divider for v1** (zero
BOM, driver doubles as the SARADC bring-up PWR-2 needs
anyway, accuracy is sufficient for a 4-step policy
ladder), with the SYS_POWER struct (§4.1) shaped so a
gauge IC can replace the source later without an ABI
change. The C6 companion also has ADCs and sits on the
battery side of some topologies — using it as the
gauge is possible but couples sensing to link health;
rejected as primary.

### 3.4 Percentage from a voltage curve — method and error

Method: sample the cell **at rest** (or correct for
load), map resting voltage through a 1S LiPo table
(indicative: 4.20 V=100%, 4.05≈90, 3.95≈80, 3.88≈70,
3.83≈60, 3.79≈50, 3.75≈40, 3.71≈30, 3.65≈20, 3.55≈10,
3.30≈3, 3.00=0 — generic curve [EST], to be replaced
by a measured discharge of the chosen cell in PWR-2),
interpolate linearly, low-pass the result.

Error sources, honestly: the mid-curve is *flat*
(~50 mV per 10% between 30–80%), so ±20 mV of noise or
sag = ±4% there; under load the cell sags by
I × ESR (a 300 mA burst into a 150 mΩ cell = 45 mV =
~9% phantom drop); temperature shifts the curve;
ageing moves the endpoints. Mitigations, in order of
value: (1) median-of-N samples + slow EMA so bursts
never move the displayed number; (2) sample-and-hold
during known-quiet moments (display off, C6 idle) for
the "committed" percentage; (3) hysteresis on every
policy threshold (§4.3) so sag can't flap the ladder;
(4) never let the *displayed* percentage rise while
discharging. A gauge IC buys its way out of (1)–(3)
with a cell model — that is the entire trade.

## 4. OS-side design

### 4.1 Syscall surface

New **`SYS_POWER` (108** — next free after FND-1's
106/107), one call, kernel fills:

```c
struct qaon_power {
    unsigned int batt_mv;      /* cell millivolts, QAON_UNKNOWN if none */
    unsigned int batt_pct;     /* 0..100, QAON_UNKNOWN if unmodelled */
    unsigned int flags;        /* bit0 present, bit1 external power,
                                  bit2 charging, bit3 gauge source
                                  (0 = SARADC model, 1 = gauge IC) */
    unsigned int soc_state;    /* 0 active, 1 idle-clocks, 2 fsm-sleep,
                                  3 off — mirrors §1.2 ladder */
    unsigned int wake_reason;  /* PMU_WAKEUP_INT_ST snapshot after a
                                  sleep/wake cycle, else 0 */
};
```

Extending `qaon_stat` instead was rejected: SYS_STAT is
a *statistics* ABI already mirrored by hand in two
places; power is a stateful subsystem (policy will
write as well as read — e.g. a future sub-call to arm a
sleep), and it deserves its own number. In QEMU the
kernel fills the struct from a **simulated source**
(§6, PWR-1): a scripted voltage curve the test harness
can drive, so the whole policy/UI stack is testable
board-less.

### 4.2 Sampling: where and how often

- The SARADC driver lives in the **kernel** (it is a
  shared, tiny, polled peripheral; the audio track set
  the precedent of kernel drivers behind syscalls).
- Sampling is **on-read with a 1 s kernel cache**: a
  SYS_POWER read triggers a conversion only if the
  cached sample is older than 1 s. One conversion is
  ~200 µs worst case; nothing samples in the
  background, so sleep states are never woken by
  gauging.
- The **policy owner is the face process**, which
  already owns the power FSM and the display token —
  it polls SYS_POWER at 1 Hz while awake (it already
  wakes at frame cadence) and acts on the ladder. No
  new daemon; qabotd/uiapp only *read* SYS_POWER for
  display. The FND-3 watchdog seam stays as designed
  there: FND-3 owns *hang* recovery, PWR owns *energy*
  recovery, and the only shared surface is the
  bootstate note API (`bootstate_note_*`) + the
  FND-1 syscalls. Neither reaches into the other.

### 4.3 Low-battery policy ladder

All thresholds on the *committed* (filtered, §3.4)
percentage, each with 2% hysteresis, each step logged
to the bootlog:

| Level | Action |
|---|---|
| ≥ 25% | normal |
| 20% | status-bar battery turns warning colour; Monitor shows LOW |
| 15% | one full-screen warning (face), display brightness step down when backlight control exists (PWR-3+; today: shorter display-on timeout) |
| 10% | "dying-gasp" preparation: sync all state (littlefs flush, bootlog note `BLE_LOWBATT` armed), notify C6 (link msg), warn user continuously |
| 5% | graceful shutdown: bootstate note (reason **4**, event 12, text "low battery") → `SYS_POWEROFF` |

The 10% pre-arm is the point: a cell that dies *hard*
at 4% leaves no energy for NAND writes, so the intent
must already be on disk. Correspondingly, a hard death
must **not** be recorded as a crash: today such a boot
simply finds `fail_count` incremented and `last_reason`
unchanged from the last explicit note (POWER on a fresh
state sector). With the ladder, an unclean boot whose
last notes contain an armed `BLE_LOWBATT` reads as
"batter
...[truncated 6658 chars]