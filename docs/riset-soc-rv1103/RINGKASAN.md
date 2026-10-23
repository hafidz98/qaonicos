# Research: RV1103 SoC capability inventory — synthesis

Date: 2026-10-23. Commission: inventory **every** capability
and feature of the Luckfox Pico Mini B's SoC (Rockchip
RV1103), so QaonicOS can exploit the silicon fully and miss
nothing. Three cluster reports carry the per-block detail
(every entry: function, register/driver source, Pico Mini B
exposure, QaonicOS status, implied abstraction, leverage):

- `BAGIAN-A-COMPUTE.md` — compute / control / security
  (23 entries + consolidated interrupt map)
- `BAGIAN-B-MEDIA.md` — media / display / audio (12 entries,
  incl. 2 documented absences)
- `BAGIAN-C-IO.md` — I/O / interconnect / memory (21 entries
  + the complete 65-node peripheral base-address table)

Method note that shapes everything below: the local V1.4
datasheet is a 25-page **brief** (full feature list + block
diagram, no register maps). The inventory therefore rests
on the brief + the vendor DTS in `hw/dts/` + the shipping
U-Boot/driver sources in `~/workspace/b0-sdk` + the board
schematic text + the QaonicOS tree. Register maps for
several blocks (crypto v3, RGA2, Rockchip timer block) are
**not held locally** — the RV1106 TRM is the known source,
and acquiring its relevant chapters is step zero of any
phase that needs them (the audio track's precedent: offsets
from the shipping driver, semantics from the TRM).

## 1. Master matrix (deduplicated)

Exposure: **R** routed on the Mini B / **D** on-die only /
**—** absent in silicon. Status: used / partial / draft /
unused / blocked.

| Block | Exp. | Status | Leverage | Part |
|---|---|---|---|---|
| Cortex-A7 + VFP/NEON | D | used, FP off (THR-1) | med | A |
| Caches / MMU | D | used | low | A |
| GIC-400 + IRQ map | D | partial — bound to QEMU addrs | **high** | A |
| TrustZone fabric | D | unused | low | A |
| Debug / JTAG | D | unused (no header) | low | A |
| HPMCU (RISC-V, camera) | D | blocked — vendor TB firmware | low | A |
| LPMCU (RISC-V, PMU) | D | unused (PWR-3 question) | low | A |
| Mailbox ×2 | D | unused | low | A/C |
| System SRAM 256 KB + PMU SRAM 8 KB | D | unused | low–med | A |
| NPU 0.5 TOPS | D | unused, no register doc | low | A |
| **Crypto engine v3** (AES-GCM/XTS, SHA, PKA 4096) | D | unused — all crypto is software | **high** | A/C |
| **TRNG** `0xff448000` | D | unused — xorshift instead | **high** | A/C |
| OTP 8 Kbit | D | unused, unread | med | A/C |
| BootROM 20 KB | D | used (B0) | low | A |
| **Watchdogs ×2 + PMU_WDT** | D | unused — FND-3 is software-only | **high** | A/C |
| Timers (arch / RK block / HPTIMER) | D | arch only; RK+HPTIMER unused | med | A/C |
| RTC `0xff1c0000` | D | unused (DTS-only evidence) | med | A/C |
| TSADC (thermal) | D | unused | med | A/C |
| PVTM ×2 | D | unused | low | A |
| GRF / IOC / pinmux | D | unused (no shared helpers) | med | A/C |
| **CRU** (4 PLL, gating, resets) | D | unused on HW | **high** (enabler) | A/C |
| PL330 DMAC | D | partial — stage 1, audio RX | high (enabler) | A/C |
| DECOM (gzip/deflate) | D | unused | low–med | A/C |
| MIPI CSI-2 ×2 + VICAP | **R** (one port, CAM FFC H1) | unused | high w/ camera | B |
| ISP 4MP@30 | D | unused | high w/ camera | B |
| VEPU H.264/H.265 + JPEG enc | D | unused | med (high w/ camera) | B |
| Video decoder | — | **absent in silicon** | none | B |
| RKIVE (22 vision units) | D | unused | med | B |
| **RGA2 2D engine** `0xff980000` | D | unused | **high** | B |
| VOP display controller | — | **no output pins (QFN88)** | none | B |
| SPI0 → ST7789 (real display path) | **R** | draft driver | (planned) | B/C |
| PWM ×11–12 | **R** (subset, collision-starved) | unused | med | B/C |
| ACODEC | nets not broken out | **used** (D1–D4, QEMU) | — | B |
| I2S0/TDM + AUD_DSM | DSM on H2 GP10/GP11 | used (I2S); DSM unused | low | B |
| PDM mic input | — | **absent in silicon** | none | B |
| GPIO (bank 2 deleted) | **R** (header subset) | partial (bank-0 set/get) | high | C |
| UART2–5 | 2/4/3 **R**; 5 not routed | unused (no HW driver) | high | C |
| I2C0/3/4 | mux-starved, ≤1 bus free | unused | med | C |
| SFC / SPI NAND | **R**, populated | draft (most complete) | high | C |
| SDMMC | **R** — populated TF socket | unused (FAT32 seam ready) | high | C |
| SDIO / eMMC controllers | not routable | unused | low | C |
| USB 2.0 OTG (DWC3) | **R** — USB-C, device-first | unused (no stack) | high (device) | C |
| GMAC + FE PHY | pairs at P401, no magnetics | unused (DTS disabled) | med-high (dev) | C |
| SARADC | IN0 reserved, IN1 free | unused (PWR owns design) | med | C |
| Memory map (SiP DDR2 64 MB) | — | QEMU map only; HW = B0.5 | foundation | C |

## 2. The exploitation map (what to build, in leverage order)

1. **CRU + GRF/IOC helpers (the universal enabler).** Every
   hardware driver in this inventory — audio included —
   begins with a CRU clock ungate + reset release that
   QaonicOS has never performed as a shared service. Small,
   board-less to design (register tables from DTS/U-Boot),
   hardware to verify. Gates: TRNG, crypto, WDT, RGA, SFC,
   SPI0, SARADC — i.e. nearly everything below.
2. **TRNG → kernel entropy service (`SYS_RANDOM`).** Small
   block, local U-Boot driver reference, and it retires a
   self-documented weakness (foundation audit #13: TLS
   entropy is timer+xorshift; B5.5 credstore nonces are
   counter-derived; the ONB-2 secret is xorshift-seeded).
   The abstraction (entropy service + simulated source)
   is board-less, PWR-1 style; the driver is hardware.
3. **Hardware watchdog → fold into FND-3.** The NS
   `snps,dw-wdt` @ `0xff5a0000` has a two-stage mode
   (interrupt first, reset on a second uncleared timeout)
   that maps directly onto bootstate's reserved
   `BSR_WATCHDOG=3`, and covers the kernel-itself-wedged
   case no software supervisor can. **Action already
   taken: the FND-3 brief will specify the software
   supervisor feeding this WDT** (QEMU side stays
   software-only; the WDT arming lands with the hardware
   phase). Missing fact: the WDT clock rate (CRU table).
4. **Crypto engine v3.** AES-GCM/XTS, SHA-256/HMAC and a
   4096-bit PKA map exactly onto shipped software paths
   (`aesgcm.c`, `updverify` SHA-256, B5.3 RSA-2048
   verify). Blocked on one procurement step: the v3
   register map (TRM or Linux rkcrypto source — not held
   locally).
5. **RGA2.** Fill/blit/alpha/scale/rotate/CSC at a
   vendor-quoted ~2000 Mpix/s — a 240×240 frame costs it
   tens of microseconds. It offloads face composition,
   uiapp, later LVGL, later camera overlays. Blockers are
   not the hardware: no local register documentation,
   physical-address buffers + cache-coherency machinery
   we don't have, and no QEMU model (driver verification
   is hardware-only; callers keep QEMU-verifiable software
   fallbacks — the QR screen stays on CPU regardless:
   RGA scaling is filtered-only, and a blurred QR is a
   broken QR). Sequence **after** the SPI display works.
6. **SDMMC driver.** The FAT32 seam already exists in the
   shell; a driver turns the TF socket into storage
   expansion + a recovery channel. Policy consequence to
   decide with it: the BootROM's SD-over-NAND boot
   priority makes a card both a recovery feature and a
   provisioning hazard (cluster C, Q6).
7. **USB device gadget (DWC3).** The board's USB-C is
   wired device-first and is already the MaskROM flashing
   path. A gadget stack is the largest single driver in
   this inventory — and the wired transport that de-risks
   provisioning (ONB), FND-5 package delivery and
   recovery. Host mode is real silicon but a later,
   larger step (and its board wiring is an open
   question).
8. **Camera pipeline (CSI-2 + ISP, then VEPU/RKIVE).**
   The product's only path to eyes. The connector is
   proven (below); the cost driver is the sensor driver +
   VICAP + RKAIQ-class ISP tuning stack — a subsystem,
   not a driver. This is a **product decision**, not an
   engineering inevitability: propose, don't schedule.
9. **GMAC bench path.** One external magnetics module
   turns the castellated P401 pairs into wired Ethernet:
   a development/bench asset that demotes the C6 from
   "only network path" to "product network path". Not a
   product feature; do not re-plan C6L around it.
10. **Small certain wins, batched:** PWM backlight dimming
    (real power + UX, one small driver; feeds PWR),
    TSADC thermal guard (its automatic reset threshold
    may serve as a software-independent last-resort
    protection for a fanless product — cluster C, Q11),
    RTC as a wall-clock source candidate (DTS-evidenced;
    domain placement caps its sleep value), DECOM for
    large payload inflate (update packages if they ever
    outgrow the FND-5 cap).

Deliberately **not** proposed: NPU (closed RKNN stack, no
register documentation, no QEMU model, no workload —
threading research verdict stands), VOP (no output pins),
video playback (no decoder in silicon), HPMCU custom code
(parked behind THR-4 ground truth), SDIO/eMMC (not
routable on this package).

## 3. Corrections register (this research vs earlier docs)

These supersede the earlier statements wherever they
conflict; each is evidence-tagged in its cluster report.

1. **A7 clock: 1.104 GHz, not 1.2 GHz.** The vendor DTS
   deletes every OPP ≥ 1.2 GHz and assigns ARMCLK =
   1.104 GHz; the V1.4 datasheet states no A7 clock.
   1.2 GHz is Luckfox marketing. (Project strings that
   print "1.2 GHz" — qfetch/SYS_STAT — are wrong until
   the clock is read from hardware; fix alongside the
   SYS_STAT work, backlog #10.)
2. **The Mini B HAS a wired TF/microSD socket.** Schematic
   shows a populated MUP M617-2 socket on SDMMC, the board
   DTS enables the controller with a DET pin group, and
   schematic text extraction shows SDMMC_DET/D0–D3/CLK/
   CMD nets routed (pins mux-shared with UART2/UART5
   functions). This replaces the B0-era conclusion ("no
   slot; pins unrouted"). Definitive confirmation =
   physical inspection when the board arrives.
3. **The camera pipeline IS reachable.** Connector H1 is
   labelled "CAM": 20-pin FFC, one 2-lane MIPI CSI port +
   MCLK0 + I2C + RST + 3V3. The matching Luckfox sensor
   is the SC3336 3 MP. (The second DPHY reaches no
   connector: one camera max.)
4. **There is no display controller path and no video
   decoder.** The shared DTS's `vop@ff990000` is RV1106
   inheritance; the RV1103 datasheet never mentions
   display and the QFN88 has zero display pins. SPI →
   ST7789 is the only display path, permanently.
5. **Mailbox count is two** (`0xff5c0000` + PMU mailbox
   `0xff378000`), not one as the datasheet brief and the
   threading research stated.
6. **Watchdog count is three in the domain tables** (NS
   dw-wdt DTS-described; a secure WDT; a PMU_WDT in
   VD_PMU) vs the brief's two.
7. **RTC exists** (`rtc@0xff1c0000`, SPI 98) per the DTS,
   despite the datasheet never mentioning one; it sits in
   VD_LOGIC per the TRM block table.
8. **Analogue audio is not broken out.** The codec's
   MIC/LINEOUT nets appear only at the SoC symbol — no
   connector or footprint; the AUD_DSM speaker output is
   on H2 pins GP10/GP11. H4 must resolve how mic/speaker
   physically attach before assuming the analogue path.
9. **DMA count discrepancy:** datasheet prose claims
   three DMACs; the DTS instantiates exactly one PL330.
   Recorded as open question, not resolved.
10. **HPMCU SRAM address discrepancy:** TOS/ini load
    address `0x210f6000` vs DTS SRAM window `0xff6c0000`
    (HPMCU partition at offset 0x3e000). Alias vs error —
    open, folds into THR-4.

## 4. Proposed phases (SOC track — unscheduled)

Each phase splits a board-less half (abstraction, tables,
simulated source, QEMU-verified callers) from a hardware
half (driver bring-up), following the PWR-1 pattern. The
track is **proposed, not scheduled** — the running order
remains FND → C6L → UI (owner, 2026-10-23); SOC phases
slot in by owner decision, except item 3 above, which is
already folded into FND-3's brief.

- **SOC-1 — Platform plumbing**: CRU gate/reset helpers,
  GRF/IOC pinmux helpers, hw-addrs.md completed from the
  65-node table, GIC binding moved from QEMU addresses to
  a per-target table (QEMU vs RV1103).
- **SOC-2 — Entropy + crypto**: TRNG driver + `SYS_RANDOM`
  entropy service (simulated source in QEMU); crypto v3
  register-map procurement, then AES-GCM/SHA/PKA offload
  behind the existing software APIs (software stays as
  fallback + QEMU path).
- **SOC-3 — Storage & wired recovery**: SDMMC driver +
  card policy (BootROM priority decision), USB device
  gadget (provisioning / update delivery / recovery
  channel).
- **SOC-4 — RGA2**: register-source procurement, driver,
  face/uiapp composition offload with software fallbacks;
  starts only after the SPI display phase works on
  hardware.
- **SOC-5 — Vision (product decision gate)**: SC3336
  sensor + VICAP + ISP path; VEPU/JPEG and RKIVE as
  follow-ons. Requires an explicit owner go.
- **SOC-6 — Bench & small wins**: GMAC + magnetics bench
  path, PWM backlight, TSADC guard, RTC source, DECOM —
  a batched hardware-phase grab-bag.

## 5. Consolidated open questions (top of 30+)

1. Crypto-v3 and RGA2 register maps: procure the RV1106
   TRM chapters / vendor driver sources into the
   workspace (gates SOC-2's second half and SOC-4).
2. Does the production PMU sleep sequence need LPMCU
   firmware, and is the HPMCU TB firmware actually
   started by our boot package? (PWR + THR-4; hardware.)
3. QEMU strategy for un-modelled blocks (RGA/ISP/VEPU/
   TRNG/crypto): accept hardware-only driver verification
   with software fallbacks, or build register-level stub
   devices into the harness first? (Affects every SOC
   phase's verification story.)
4. SDMMC: DET polarity on the real socket + the
   SD-over-NAND boot-priority policy (recovery feature
   vs provisioning hazard).
5. I2C bus arbitration once SPI0/UART4/SDMMC/SARADC-IN1
   are committed: touch and a future gauge IC both want
   the one realistically-free bus (cluster C, Q7).
6. WDT clock rate from the CRU assignment table
   (needed to express FND-3 timeouts in seconds).
7. SPI0 clock ceiling (TRM figure) → the ST7789
   full-frame write rate the UI track's pacing question
   is waiting on (cluster C, Q10).
