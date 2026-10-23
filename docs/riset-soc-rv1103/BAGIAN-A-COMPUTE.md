# RV1103 SoC Capability Inventory — Part A: Compute, Control, Security

Cluster A of the exhaustive RV1103 inventory for QaonicOS.
Scope: everything in the SoC that computes, controls, or
secures — CPU complex, the two RISC-V companion blocks,
mailbox/SRAM fabric, NPU, crypto/TRNG/OTP, watchdogs,
timers, RTC, thermal/process monitors, clock/reset
infrastructure, and the interrupt map. Media, connectivity
and pin-level blocks are covered by the other clusters;
blocks that sit on the boundary are listed in §25 so
nothing in the datasheet diagram is silently skipped.

## Sources and evidence levels

- **[DS]** — *Rockchip RV1103 Datasheet V1.4* (2023-07-25),
  local copy `~/workspace/luckfox-pico-mini-b/documents/`.
  A 25-page brief datasheet: feature list (§1.2), block
  diagram (Fig. 1-1, §1.3), electricals. It contains **no
  register maps and no clock figures for the A7**.
- **[SDK]** — vendor device trees held locally in
  `~/workspace/qaonic_os/hw/dts/` (`rv1106.dtsi`,
  `rv1103.dtsi`, `rv1106-amp.dtsi`, U-Boot DTS), the
  U-Boot source tree in
  `~/workspace/b0-sdk/luckfox-pico/sysdrv/source/uboot/`,
  and rkbin (`~/workspace/b0-sdk/rkbin/`). RV1103 inherits
  `rv1106.dtsi` almost wholesale, so most base addresses
  and IRQ numbers below are rv1106-node values that the
  RV1103 overlay does not override [SDK: rv1103.dtsi].
- **[TRM via PR]** — RV1106 TRM facts as established by
  `docs/riset-power-battery/RINGKASAN.md` (esp. ch. 7 PMU).
  No TRM PDF is held locally; these claims are only as
  strong as that research.
- **[TR]** — `docs/riset-threading-compute/RINGKASAN.md`.
- **[code]** — the QaonicOS tree (`kernel/kernel/arm/`,
  `user/`).
- **[inf]** inference, **[unknown]** not determinable
  from local materials.

A general warning that applies to every entry: the
QaonicOS kernel today runs on QEMU `-M virt`, and several
drivers bind QEMU's addresses, not the RV1103's. Where
that is the case it is stated explicitly.

---

## 1. Cortex-A7 core (with VFP/NEON)

- **What it does** — The application processor: single-core
  ARM Cortex-A7, full ARMv7-A, NEON Advanced SIMD and a
  separate FPU integrated [DS §1.2.1]. One isolated voltage
  domain (VDD_ARM) to support DVFS [DS §1.2.1].
- **Registers/driver** — ARM architectural registers; no
  SoC driver needed. DVFS operating points come from the
  vendor OPP table [SDK: rv1106.dtsi `cpu0_opp_table` =
  408/600/816/1104/1200/1296/1416/1512/1608 MHz], of which
  **rv1103.dtsi deletes every point ≥ 1.2 GHz and assigns
  ARMCLK = 1,104,000,000 Hz** [SDK: rv1103.dtsi]. The
  datasheet itself never states an A7 clock [DS §1.1–1.2].
- **Pico Mini B exposure** — The SoC itself; n/a.
- **QaonicOS status** — Used: it is the only CPU the kernel
  runs on. VFP/NEON state is the known trap summarised from
  [TR] THR-1 (not redone here): `fpu.c` has enabled CP10/CP11
  since M3, but `fpu_save`/`fpu_restore` are empty stubs,
  `user_ctx` carries no FP state, and no build target passes
  `-mfpu`/`-mfloat-abi` — so FP codegen must stay off until
  THR-1 lands, or FP registers corrupt silently across
  daemon switches [code: kernel/kernel/arm/fpu.c].
  DVFS is unused: the kernel makes no CRU/OPP changes.
- **Abstraction it implies** — CPU itself: none new. DVFS
  would ride the CRU abstraction (§21) plus an OPP policy
  in the kernel; no syscall needed for internal use.
- **Leverage: medium** — NEON enablement is THR-1's win
  (face/audio math); DVFS is a power-track lever, not a
  capability gap for the current product.

## 2. Caches and MMU

- **What it does** — 32 KB L1 instruction + 32 KB L1 data
  caches, unified 128 KB L2 [DS §1.1, §1.2.1]; ARMv7-A MMU
  with the standard two-level page tables.
- **Registers/driver** — ARM architectural (CP15);
  L2 is a unified cache internal to the core complex —
  no separate L2 controller node exists in the DTS
  [SDK: rv1106.dtsi has no l2-cache controller node].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Used: Mach's `pmap.c` drives the
  MMU (per-program page tables, the basis of the fault
  isolation in `user_fault`) [code: kernel/kernel/arm/
  pmap.c]. Cache maintenance is whatever the Mach port
  inherited; no inventory of cache/TLB policy exists in
  project docs [unknown].
- **Abstraction it implies** — Existing Mach VM; nothing
  new.
- **Leverage: low** — Works; only a documentation gap
  (cache policy on DMA buffers becomes real the day
  PL330 moves product data — see §22).

## 3. GIC-400 interrupt controller and the IRQ map

- **What it does** — ARM GIC-400 (GICv2): distributes all
  SoC interrupts to the A7 (and, per §1.2.3, interrupt
  lines are also connected to the MCU's controller).
  Datasheet figure: **121 SPI sources**, 16 software
  interrupts, per-source programmable priority, high-level
  or rising-edge sensitivity [DS §1.2.3].
- **Registers/driver** — GICD `0xff1f1000`, GICC
  `0xff1f2000` (plus virtualisation slots at 0xff1f4000/
  0xff1f6000), compatible `arm,gic-400`
  [SDK: rv1106.dtsi; docs/hw-addrs.md]. The DTS assigns
  72 distinct SPI numbers in rv1106.dtsi alone, with
  assignments running up to SPI 126 across the local DTS
  set [SDK] — the per-peripheral SPI numbers are cited
  in each entry below.
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Partial, with a hardware caveat:
  `gic.c` exists and works, but it is bound to **QEMU
  virt's GIC (GICD 0x08000000 / GICC 0x08010000)**, and the
  only source used is the timer PPI [code: gic.c header].
  On the board, the driver must be re-pointed at
  0xff1f1000/0xff1f2000 and the SPI routing table built
  from the DTS assignments. No QaonicOS code consumes any
  SoC SPI today (all device I/O is polled or arch-timer
  driven).
- **Abstraction it implies** — Existing `gic_enable_irq()`
  internal API, extended with an RV1103 base-address
  build option and an SPI enable/priority call for future
  drivers (audio DMA, UART, WDT all need it).
- **Leverage: high** — Every interrupt-driven hardware
  feature on the board (audio PL330 IRQs, WDT first-stage
  interrupt, crypto/TRNG completion, mailbox) waits on
  this being real at the RV1103 addresses.

## 4. TrustZone and the SoC security architecture

- **What it does** — ARM TrustZone on the A7 plus a SoC-
  wide security fabric: masters other than the CPU are
  software-programmable secure/non-secure; some slaves are
  secure-only, others programmable; part of the system
  SRAM is secure-only; external DDR can be split into 16
  regions, each programmable per-master [DS §1.2.3
  "Secure System"]. "Secure OS" support is claimed
  [DS §1.2.3], and rkbin carries an OP-TEE TA image for
  this chip (`rv1103b_tee_ta_v1.05.bin`) [SDK: rkbin].
- **Registers/driver** — Firewall/GRF-style programming;
  no register documentation is held locally [unknown].
  Our boot chain does not package a TEE (the B0 ini uses
  the non-TEE loader set) [SDK: rkbin ini, via TR §2.1].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused. The kernel runs as a
  single privilege world; there is no secure monitor, no
  OP-TEE, no region programming. The product's security
  model (B5) is application-level: signed updates,
  encrypted credstore, PIN gate — none of it uses TZ.
- **Abstraction it implies** — None proposed. Adopting TZ
  would be an architecture project (a secure-world
  firmware + SMC ABI), not a driver.
- **Leverage: low** — Real capability, wrong size for
  this OS today; recorded so the inventory is complete
  and so nobody mistakes the B5 mechanisms for TZ.

## 5. Debug: CoreSight / JTAG (CPU and HPMCU)

- **What it does** — The A7 has its ARMv7 debug
  architecture; the MCU has an "Integrated Debug
  Controller with JTAG interface" [DS §1.2.3]. The
  datasheet also lists "secure debug" under Secure
  System [DS §1.2.3] (debug can be locked by the secure
  world / OTP policy — stated neutrally as inventory).
- **Registers/driver** — JTAG is a pin function, not an
  MMIO driver. The schematic's pin-mux tables show
  `JTAG_CPU_TCK/TMS` and `JTAG_HPMCU_TCK/TMS` as mux
  options on shared balls (muxed against UART5/SDMMC on
  one set, UART2 on another) [SDK: schematic.pdf text].
- **Pico Mini B exposure** — **Unknown / effectively not
  routed**: the JTAG signals exist only as alternate
  functions of balls the board uses for SDMMC/UART; no
  dedicated JTAG header is documented for the Mini B in
  local materials [unknown].
- **QaonicOS status** — Unused (kernel debugging is
  serial + QEMU gdbstub in emulation).
- **Abstraction it implies** — None.
- **Leverage: low** — A bring-up convenience on the bench,
  not a product capability.

## 6. HPMCU — the RISC-V camera fast-boot core

Summary of [TR] §2.1 plus the DTS facts it did not use;
not a re-investigation.

- **What it does** — A RISC-V MCU inside VD_CORE with
  16 KB cache and its own programmable interrupt
  controller (all IRQ lines are also connected to it)
  [DS §1.2.3]. Rockchip's pattern is camera fast boot:
  the MCU starts the sensor + ISP and produces first
  frames while the A7 still loads its OS [TR, SDK].
- **Registers/driver** — No register map is held locally.
  What exists: rkbin images `rv1103b_hpmcu_tb_v1.00.bin`
  (test board — **the one our B0 idbloader ini loads as
  LOADER3**) and `rv1103b_hpmcu_wrap_v2.02.bin`
  (TEE-packaged) [SDK: rkbin/bin/rv11, ini]. The DTS AMP
  node shows the clocks the vendor model gives it:
  `CLK_CORE_MCU`, `CLK_CORE_MCU_RTC`, timer5 and the
  mailbox clock [SDK: rv1106-amp.dtsi]. The vendor's own
  firmware model is an RT-Thread tree that is **not
  checked out locally** [TR].
- **Pico Mini B exposure** — On-die; its camera purpose
  would need the CSI path (other cluster). Nothing to
  route.
- **QaonicOS status** — Blocked/unexamined: closed vendor
  test-board firmware may already be started by our boot
  chain; what it owns while our kernel runs is unverified
  (THR-4, hardware) [TR].
- **Abstraction it implies** — None until THR-4 ground
  truth exists. Any future use starts with the mailbox
  (§8), not with code execution.
- **Leverage: low** — No product need the A7 or the
  ESP32-C6 does not cover; the actionable item is the
  THR-4 audit of what is already running, not adoption.

## 7. LPMCU — the PMU's RISC-V sequencer

- **What it does** — A small RISC-V core inside VD_PMU
  that sequences the PMU's *software* sleep path (TRM
  §7.6.2): it runs while the main SoC is powering down/up
  [TRM via PR; TR §2.2]. It is a power-management
  mechanism, not compute capacity.
- **Registers/driver** — Programmed by PM firmware, not
  by an OS driver; whether the production sleep sequence
  even needs LPMCU firmware, or the pure hardware-FSM
  path (TRM §7.6.1) suffices, is the power track's open
  question Q2 — no rv1103b PM/suspend blob exists in
  rkbin, hinting FSM-only [TR; SDK: rkbin listing].
- **Pico Mini B exposure** — On-die (VD_PMU, always-on
  domain) [TRM via PR §1.1].
- **QaonicOS status** — Unused; relevant only to PWR-3
  (real SoC sleep).
- **Abstraction it implies** — None. If the FSM path
  suffices, the LPMCU never appears in QaonicOS at all.
- **Leverage: low** — Its value is entirely inside the
  PWR-3 sleep implementation decision.

## 8. Mailbox(es) — CPU ↔ MCU inter-processor communication

- **What it does** — Hardware message boxes between the
  A7 and the MCU: four elements, each with one data word,
  one command word and a flag bit that can raise an
  interrupt, plus 32 lock registers for software mutual
  exclusion [DS §1.2.3].
- **Registers/driver** — **Two instances exist in the
  DTS**, refining the datasheet's "one Mailbox": the main
  `mailbox@0xff5c0000` (0x200, **SPI 1**, compatible
  `rockchip,rv1106-mailbox`/`rk3368-mailbox`) and a
  `pmu_mailbox@0xff378000` (0x200, **SPI 114**) in the PMU
  area [SDK: rv1106.dtsi]. The rk3368 lineage means the
  register layout follows Rockchip's long-standing
  mailbox design [inf]. No Rockchip mailbox driver is in
  the local U-Boot tree (only the generic uclass +
  Tegra) [SDK: uboot drivers/mailbox]; no message
  *protocol* (semantics) is documented in local materials
  [TR].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused. Nothing in the kernel
  touches either instance.
- **Abstraction it implies** — A kernel `mbox_send/recv`
  pair would be the prerequisite for any HPMCU
  relationship beyond coexistence; no user syscall is
  implied (this is kernel-internal plumbing).
- **Leverage: low** — Only as valuable as the HPMCU
  decision (§6); on its own it talks to firmware we do
  not control.

## 9. System SRAM (256 KB) and PMU SRAM (8 KB)

- **What it does** — 256 KB of on-chip shared SRAM
  [DS §1.2.2], part of it addressable only in secure mode
  [DS §1.2.3]; plus a separate 8 KB PMU SRAM in VD_PMU
  [DS Fig. 1-1; TRM via PR].
- **Registers/driver** — Plain MMIO RAM, no driver. The
  DTS maps the A7-visible window at **`0xff6c0000`,
  256 KB**, partitioned as `rkisp-sram@0` (0x3e000 =
  248 KB) and `hpmcu-sram@0x3e000` (0x2000 = 8 KB)
  [SDK: rv1106.dtsi `system_sram`]. Note the discrepancy:
  [TR] records the TOS wrap ini loading the HPMCU image
  at `0x210f6000`; how that address relates to the DTS
  window (an MCU-side alias is the obvious candidate) is
  **not resolvable from local materials** [unknown].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused. The kernel uses SiP DDR2
  for everything; the SRAM (including the 8 KB carved
  for the HPMCU) is untouched — and, per §6, possibly
  written by the vendor MCU firmware we boot.
- **Abstraction it implies** — A reserved SRAM region
  could serve as a fast, DDR-independent scratch (e.g.
  crash/panic records that survive a warm reset — the
  bootstate design currently uses blk sectors for this).
  Would be a kernel-internal allocation, no new syscall.
- **Leverage: low–medium** — The warm-reset-surviving
  scratch idea is genuinely useful for the resilience
  story, but the blk-based mechanism already works.

## 10. NPU (0.5 TOPS)

Hardware facts only; the adoption verdict lives in
[TR] §2.3 and is not reopened here.

- **What it does** — Neural-network accelerator, up to
  0.5 TOPS, INT4/INT8/INT16 hybrid operation, custom
  operators, models converted from TensorFlow/Caffe/
  TFLite/PyTorch/ONNX [DS §1.1, §1.2.5].
- **Registers/driver** — DTS node `npu@0xff660000`
  (0x10000 window, **SPI 109**, clocks ACLK_RKNN at an
  assigned 420 MHz) [SDK: rv1106.dtsi]. **No register
  documentation exists in any local material** — no TRM
  chapter, no driver source in the local U-Boot tree,
  and the Linux RKNN driver/runtime is not checked out
  locally [unknown/SDK]. The only vendor path is the
  closed RKNN-Toolkit2 + `librknnrt` + kernel driver
  stack [TR].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused, and unusable from held
  materials (no register doc, no QEMU model, no current
  workload) [TR].
- **Abstraction it implies** — None without the vendor
  stack.
- **Leverage: low** — Recorded for completeness; the
  facts above are what a future re-evaluation would
  start from.

## 11. Crypto engine (cipher + hash + PKA)

- **What it does** — One cipher engine with LLI DMA
  transfer: SHA-1, SHA-256/224, MD5 and HMAC (hardware
  padding); AES-128/192/256 in ECB/CBC/OFB/CFB/CTR/CTS/
  **XTS/GCM**/CBC-MAC/CMAC; DES/TDES; and a PKA unit
  doing mathematical operations up to **4096-bit for
  RSA/ECC/SM2** [DS §1.2.3 "Secure System"].
- **Registers/driver** — DTS: `crypto@0xff440000`
  (0x2000, **SPI 15**, compatible `rockchip,crypto-v3`;
  core + PKA clocks assigned 200 MHz) [SDK: rv1106.dtsi].
  **Register-map source locally is incomplete**: the
  U-Boot tree carries Rockchip crypto v1/v2 drivers
  (`drivers/crypto/rockchip/crypto_v1.c`, `crypto_v2.c`)
  but no v3; the Linux v3 driver is not checked out
  locally [SDK]. A driver therefore needs the TRM or
  the Linux source, neither held.
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused. All cryptography today is
  software: AES-256-GCM (`aesgcm.c`), SHA-256
  (`updverify.c`, reused by webui/credstore), RSA-2048
  verify for signed updates (B5.3) — all correct, all
  CPU-bound [code]. The PKA's RSA verify and the AES-GCM
  mode are exact matches for the two hottest of those.
- **Abstraction it implies** — Kernel-internal crypto
  service first (hash + AEAD + PKA verify) consumed by
  credstore/updverify; a user-facing `SYS_CRYPTO` only
  if qabotd later needs it.
- **Leverage: high** — Directly accelerates existing,
  shipped security paths (update signature verify,
  credential store) instead of enabling a speculative
  feature; gated on obtaining the v3 register map.

## 12. TRNG / random number generators

- **What it does** — Hardware random number generation:
  the datasheet describes **two engines** — "one secure
  only engine, another one security configurable"
  [DS §1.2.3].
- **Registers/driver** — The DTS describes **one** node:
  `rng@0xff448000` (0x200, **SPI 17**, compatible
  `rockchip,trngv1`, clock `HCLK_TRNG_NS` — the NS
  suffix marks it as the non-secure-facing instance)
  [SDK: rv1106.dtsi]. Whether the secure-only engine is
  the same block's secure face or a second block the DTS
  omits is not determinable locally [unknown]. A driver
  source exists locally: U-Boot `drivers/rng/
  rockchip_rng.c` (+ `rockchip_rand.c`) [SDK].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused, and its absence is a
  documented weakness: TLS entropy is timer + xorshift,
  self-labelled weak in the code ("LEMAH … hanya untuk
  uji") [code: user/tls/tls_port.c]; credstore nonces
  are derived from SHA-256 over counters/time rather
  than from an entropy source [code: credstore.c].
  Foundation audit item #13 is exactly this gap.
- **Abstraction it implies** — A small kernel entropy
  service + `SYS_RANDOM` (or an internal `kentropy`
  consumed by credstore and a TLS port hook), with the
  QEMU build keeping a clearly-marked fallback.
- **Leverage: high** — The cheapest real security upgrade
  in this cluster: one small block, a local driver
  reference, and it retires a known-weak RNG from the
  TLS and credential paths.

## 13. OTP (one-time programmable memory)

Inventory only. (Any *use* of OTP for secure boot is an
owner-deferred topic; this entry records hardware facts
and proposes nothing.)

- **What it does** — 8 Kbit of one-time-programmable
  storage, of which 7 Kbit are designated for secure
  applications; Program/Read/Idle modes [DS §1.2.12].
  Programming is irreversible by construction.
- **Registers/driver** — DTS: `otp@0xff3d0000` (0x4000)
  with factory data cells already defined: cpu-code
  @0x02, cpu-version @0x08, a 16-byte chip **id @0x0a**,
  cpu/logic leakage bytes @0x1a/0x1b, MAC-PHY trim
  @0x2d [SDK: rv1106.dtsi]. Driver sources exist
  locally: U-Boot `drivers/misc/rockchip-otp.c` and an
  rv1106-specific secure-OTP shim
  (`rv1106-secure-otp.S`) [SDK]. The block sits in
  VD_LOGIC [TRM via PR §1.1].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused and unread: the kernel
  does not even read the factory chip ID (device
  identity today comes from a generated littlefs secret
  — see ONB-2's documented deviation).
- **Abstraction it implies** — A read-only `SYS_OTP_READ`
  (or kernel-internal read at boot) is all the current
  product could want, e.g. a real per-device ID for the
  onboarding payload. Any write path is out of scope
  here by the owner's deferral.
- **Leverage: medium** — Read-only use is cheap and
  fixes a real identity wart; anything beyond reading
  is explicitly not this document's business.

## 14. BootROM and secure-boot ROM facts (inventory only)

- **What it does** — 20 KB mask ROM [DS Fig. 1-1]. Boots
  the system from SPI (flash/NAND via FSPI) or SD/MMC,
  and supports code download over USB or UART (the
  MaskROM recovery path the B0 flashing flow relies on —
  the board cannot be bricked past this) [DS §1.2.2;
  project B0 records]. rkbin also ships rv1103b USB-plug
  images and an OP-TEE TA image (§4) [SDK: rkbin].
- **Registers/driver** — ROM code; no driver. Loader
  packaging (idbloader stages: DDR bin, SPL, optionally
  the HPMCU image as LOADER3) is rkbin tooling
  [SDK; TR §2.1].
- **Pico Mini B exposure** — The USB download path is
  the board's USB-C port (other cluster); boot media is
  the onboard SPI NAND.
- **QaonicOS status** — Used implicitly every boot
  (B0 chain: MaskROM → idbloader → U-Boot → kernel).
  No QaonicOS code interacts with it beyond that.
- **Abstraction it implies** — None.
- **Leverage: low** — Inventory facts; the recovery
  property is already load-bearing in the B0 design and
  needs nothing new.

## 15. Watchdogs (NS WDT, secure WDT, PMU_WDT)

- **What it does** — 32-bit down-counter watchdogs. On
  timeout a WDT can (a) generate a system reset
  directly, or (b) **raise an interrupt first; only if
  the service routine has not cleared it by the second
  timeout does it reset** [DS §1.2.3]. The datasheet
  counts **two**: one for non-secure applications, one
  for secure [DS §1.2.3]. The TRM power-domain table
  adds a third, a **PMU_WDT inside VD_PMU** (always-on)
  [TRM via PR §1.1].
- **Registers/driver** — The DTS exposes only the
  non-secure one: `watchdog@0xff5a0000` (0x100,
  **SPI 46**, compatible `rockchip,rv1106-wdt` /
  **`snps,dw-wdt`**, clocks `TCLK_WDT_NS`/`PCLK_WDT_NS`)
  [SDK: rv1106.dtsi]. It is therefore a Synopsys
  DesignWare WDT — a publicly documented register map —
  and a driver source is held locally: U-Boot
  `drivers/watchdog/rockchip_wdt.c` [SDK]. The secure
  WDT and PMU_WDT have no DTS nodes; their register
  locations are TRM-only [unknown locally]. Timeout
  range is set by the 32-bit counter against the WDT
  clock; the exact TCLK frequency is a CRU assignment
  not stated in local materials [unknown], so ranges
  are quoted as "counter-width × TCLK period" rather
  than seconds.
- **Pico Mini B exposure** — On-die; the NS WDT and the
  PMU_WDT are in different power domains (main vs
  VD_PMU always-on) [SDK; TRM via PR]. Domain matters:
  only the PMU_WDT can supervise sleep states.
- **QaonicOS status** — Unused in hardware. FND-3 is
  building a *software* supervisor (per-daemon heartbeat
  + escalation to SYS_RESET); bootstate already reserves
  reset reason **BSR_WATCHDOG = 3** [code: bootstate.c].
  Facts FND-3 asked this inventory for: a hardware WDT
  exists that (1) can fire an interrupt before resetting
  — the natural hook for the supervisor to log a
  CRASHMARK with reason 3 and then let the reset happen;
  (2) resets even if the kernel itself is wedged, which
  a software supervisor by construction cannot do for
  itself; (3) in QEMU it does not exist (virt has no
  dw-wdt), so the software supervisor remains the
  testable layer and the WDT is a hardware-phase
  complement, not a replacement.
- **Abstraction it implies** — Kernel-internal
  `wdt_arm(seconds)` / `wdt_pet()` owned by the FND-3
  supervisor; no user syscall.
- **Leverage: high** — Completes the recovery chain the
  out-of-the-box goal promises ("never a reset-loop
  brick" also needs the kernel-wedged case covered),
  with a public register map and a local driver
  reference.

## 16. Timers: ARM arch timer, Rockchip timer block, HPTIMER

Three distinct timer resources; only the first is used.

- **ARM generic timer** — `arm,armv7-timer`, accessed via
  PPI, no MMIO [SDK: rv1106.dtsi; docs/hw-addrs.md].
  **Status: used** — `clock.c` runs the 100 Hz tick from
  the virtual timer PPI (27) on QEMU [code: clock.c].
  On hardware the physical timer PPI is the equivalent.
  Leverage: n/a (load-bearing already).
- **Rockchip timer block** — **2 secure timers + 6
  non-secure timers, each a 64-bit counter** with
  free-running and user-defined-count modes and
  interrupt operation [DS §1.2.3]. Existence is
  corroborated by clocks (`CLK_TIMER_ROOT`, `PCLK_TIMER`,
  `CLK_TIMER5` in the AMP node) [SDK: rv1106-amp.dtsi],
  but **no MMIO node for the block exists in the local
  DTS**, so its base address is not determinable from
  local materials [unknown — TRM territory].
  **Status: unused. Abstraction:** kernel-internal
  one-shot/deadline timers would serve the FND-3
  supervisor deadlines and audio pacing better than
  tick counting. **Leverage: medium** (gated on the
  register map).
- **HPTIMER** — A timer in **VD_PMU** (always-on), listed
  among the PMU wake sources (wake source 5: "HPTIMER
  interrupt — timed wake") [TRM via PR §1.1, wake table].
  No DTS node locally [unknown base]. **Status: unused;
  it is the timed-wake mechanism PWR-3 will need** if
  RTC cannot wake the SoC from VD_PMU states (§17).
  **Leverage: medium** — small block, but it is the
  difference between "wakes on a schedule" and "wakes
  only on a button" in deep sleep states.

## 17. RTC

- **What it does** — A real-time clock block
  (`rockchip,rv1106-rtc`) exists in the vendor DTS:
  `rtc@0xff1c0000` (0x1000, **SPI 98**, status disabled),
  clocked by `PCLK_VI_RTC_PHY`/`PCLK_VI_RTC_TEST` at an
  assigned 24 MHz [SDK: rv1106.dtsi].
- **The datasheet never mentions it** — the V1.4 feature
  list and block diagram contain no RTC at all
  [DS §1.2, Fig. 1-1]. Its presence rests on the DTS
  [SDK] and on the TRM power-domain table, which places
  **RTC in VD_LOGIC** [TRM via PR §1.1] — i.e. *not* in
  the always-on VD_PMU, so whether it keeps time or can
  wake the SoC from the deeper PMU states is doubtful;
  the power research already concluded timed wake
  should rely on HPTIMER / PMU timeout instead
  [TRM via PR]. The "VI" in its clock names further
  suggests it hangs off the video-input clock domain
  [inf].
- **Pico Mini B exposure** — On-die; a battery-backed
  RTC function would additionally need its domain
  powered, which the board does not provide [inf].
- **QaonicOS status** — Unused. Wall-clock time today is
  set by NTP (`SYS_TIME_SET` path) and is not persistent
  across power loss; the F2/UTIMESET work verified the
  set path, not a hardware RTC.
- **Abstraction it implies** — A `SYS_RTC` read/write
  behind the existing time syscalls, used at boot to
  seed the wall clock before NTP.
- **Leverage: medium** — Real but bounded: it fixes
  "clock is wrong until network" only while power is
  maintained, and its VD_LOGIC placement caps its
  value for sleep/wake.

## 18. TSADC — temperature sensor

- **What it does** — On-die temperature sensor, −40…125 °C
  at 1 °C resolution, with a user-defined (software-
  triggered) mode and an automatic mode in which
  high/low alarm interrupts are configurable and a
  **system-reset temperature threshold** can be set
  [DS §1.2.12; electricals §3.10].
- **Registers/driver** — DTS: `tsadc@0xff3c8000`
  (0x1000, **SPI 97**), clocks at 1 MHz / 12 MHz
  assigned, and the vendor policy baked in:
  `hw-tshut-temp = 120000` (120 °C) with tshut mode 0 =
  via CRU [SDK: rv1106.dtsi]. No driver source is held
  locally (Rockchip TSADC register layouts are a
  long-standing family design [inf]); the register map
  proper is TRM material [unknown locally].
- **Pico Mini B exposure** — On-die sensor; n/a.
- **QaonicOS status** — Unused: nothing reads die
  temperature, and SYS_STAT has no temperature field
  (the Monitor screen's unknowns are cpu/blk today).
- **Abstraction it implies** — A kernel sensor read
  feeding SYS_STAT (die temperature beside the existing
  fields) + enabling the hardware tshut as a safety net.
- **Leverage: medium** — An unattended, fanless,
  sealed-box device with a documented thermal-reset
  peripheral it never arms is leaving a safety feature
  on the table; the read path is also trivially
  demonstrable (die temp in Monitor/qfetch).

## 19. PVTM and PVTPLL — process/voltage/temperature monitors

- **What it does** — PVTM blocks measure silicon process
  speed / voltage / temperature characteristics on-die;
  Rockchip's DVFS stack uses them to grade the chip and
  pick safe voltage/frequency points (the OTP leakage
  cells in §13 are their calibration companions). The
  block diagram also shows a dedicated **PVTPLL**
  [DS Fig. 1-1].
- **Registers/driver** — Two instances in the DTS:
  core PVTM `pvtm@0xff240000` (`rv1106-core-pvtm`) and
  PMU PVTM `pvtm@0xff390000` (`rv1106-pmu-pvtm`)
  [SDK: rv1106.dtsi]. No feature text in the datasheet
  beyond the diagram [DS]; no local driver.
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused; DVFS itself is unused
  (§1), so there is no consumer.
- **Abstraction it implies** — None standalone; a data
  source inside any future DVFS policy.
- **Leverage: low** — Only matters if DVFS becomes a
  power-track goal.

## 20. GRF — general register files (control plumbing)

- **What it does** — The SoC's miscellaneous system-
  control register block (mode selects, status, assorted
  peripheral configuration). Several peripherals hold a
  `rockchip,grf` phandle (RTC, TSADC in the DTS) because
  part of their configuration lives here [SDK].
- **Registers/driver** — `syscon@0xff000000`, size
  0x68000, including a `grf_cru` sub-block
  [SDK: rv1106.dtsi; docs/hw-addrs.md]. (Pin control
  proper lives in the separate IOC syscon
  `0xff538000` + PMU IOC — pin-level, other cluster.)
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused on hardware (no driver
  needs it under QEMU). It will be touched incidentally
  by the first drivers that do (TSADC, RTC).
- **Abstraction it implies** — Kernel-internal register
  access helpers; never a syscall.
- **Leverage: low** — Plumbing, recorded so its absence
  from the driver picture is a known fact, not a
  surprise mid-driver.

## 21. CRU — clock and reset unit

- **What it does** — Generates and gates every clock in
  the SoC: **4 PLLs** fed by a 24 MHz oscillator, clock
  gating control for individual components, a global
  soft reset, and per-component soft resets
  [DS §1.2.3]. The DTS names PLL_GPLL/PLL_CPLL (and the
  power research's domain table adds the A/D PLLs)
  [SDK: rv1106.dtsi; TRM via PR].
- **Registers/driver** — `clock-controller@0xff3a0000`,
  window 0x20000, `#clock-cells`/`#reset-cells` — every
  peripheral entry in this document references CRU clock
  and reset IDs [SDK: rv1106.dtsi]. The reset side also
  includes the NPOR/power-good block: the DTS defines an
  `rknpor-powergood` node on **SPI 86** [SDK], and
  NPOR sits in VD_PMU [TRM via PR].
- **Pico Mini B exposure** — On-die; n/a. (The 24 MHz
  oscillator is a board crystal — present on the Mini B
  by construction [inf].)
- **QaonicOS status** — Unused on hardware: under QEMU
  there is no CRU to program, and the kernel changes no
  clock or reset register. This is the single biggest
  "the OS has never touched the real chip" fact in this
  cluster: **every future hardware driver starts with a
  CRU ungate + soft-reset sequence**, and the audio
  track already carries a TODO(HW) for exactly this
  (CRU PERICLKGATE/CKR for the codec MCLK).
- **Abstraction it implies** — A kernel `cru_clk_enable
  (id)` / `cru_reset(id)` pair using the DTS clock/reset
  numbering, written once and shared by all hardware
  drivers; policy (DVFS, gating for power) layers on top
  in the power track.
- **Leverage: high** — Not a feature, but the mandatory
  first step of all hardware enablement in this cluster
  and beyond; doing it once, deliberately, beats five
  drivers each inventing it.

## 22. DMAC — PL330 DMA controllers

- **What it does** — Microcode-programmed DMA with
  linked-list scatter-gather, memory↔memory and
  memory↔peripheral transfers. The datasheet counts
  **three** DMA controllers for the peripheral system,
  each 8 channels, 32 peripheral hardware requests, 2
  interrupt outputs, per-channel TrustZone state
  [DS §1.2.3].
- **Registers/driver** — The DTS describes **one**
  general-purpose controller: `dma-controller@
  0xff420000` (0x4000), `arm,pl330`, with nine IRQ
  lines (**SPI 63–70 + 79**) [SDK: rv1106.dtsi]. The
  other two controllers the datasheet counts are not
  separate DTS nodes (presumed internal to media
  subsystems [inf]). The PL330 register/microcode
  architecture is ARM-public.
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — **Partial, and ahead of most of
  this cluster**: `pl330.c` already targets the real
  RV1103 base **0xff420000** with a 28-byte RX microcode
  program and syscalls 89–92 (D2b stage 1, UPL330 PASS
  for structure). Completion is hardware-gated: the
  DMAGO kick and interrupt completion can only be
  proven on the board, and cache maintenance policy for
  DMA buffers (§2) is unwritten.
- **Abstraction it implies** — Existing syscalls 89–92;
  a channel-completion (interrupt) model is the missing
  piece, riding §3's SPI enablement.
- **Leverage: medium** — Already started; it is the
  data-plane workhorse for audio (and later C6-UART and
  NAND) once interrupts work.

## 23. DECOM — hardware decompression engine

- **What it does** — Hardware decompression of **GZIP /
  DEFLATE / ZLIB** streams, with a decompressed-size
  limit function specifically to stop decompression
  bombs from destroying memory [DS §1.2.3].
- **Registers/driver** — DTS: `decompress@0xff520000`
  (0x1000, **SPI 112**, ACLK/DCLK/PCLK clocks)
  [SDK: rv1106.dtsi]. A driver source exists locally:
  U-Boot `drivers/misc/rockchip_decompress.c` [SDK].
- **Pico Mini B exposure** — On-die; n/a.
- **QaonicOS status** — Unused; the OS has no compressed
  payload path today (updates are raw payloads in QUP1
  packages; assets are compiled in).
- **Abstraction it implies** — A kernel decompression
  service behind the update path (compressed update
  payloads → smaller NAND/flash footprint) or a
  `SYS_DECOMPRESS` if user programs ever need it.
- **Leverage: medium** — Real but conditional: it pays
  off the day update packages or on-device assets get
  big enough that software inflate on a 1.1 GHz A7
  hurts; the bomb-limit is an unusually good fit for the
  B5 threat model.

## 24. Interrupt map — consolidated facts

- The GIC accepts **121 SPI sources** per the datasheet
  [DS §1.2.3]; the vendor DTS assigns numbers across
  that space (72 distinct SPIs in rv1106.dtsi; highest
  seen locally SPI 126) [SDK].
- SPIs this cluster depends on, all [SDK: rv1106.dtsi]:
  mailbox 1, crypto 15, RNG 17, WDT 46, DMAC 63–70 + 79,
  NPOR power-good 86, TSADC 97, RTC 98, NPU 109,
  DECOM 112, PMU mailbox 114.
- Priorities are per-source programmable [DS]; the
  current kernel sets none on hardware (it runs QEMU's
  map). Building the RV1103 SPI table — number, edge/
  level, priority — from the DTS is a defined, finite
  piece of bring-up work folded into §3.

## 25. Boundary blocks (present in the diagram; other clusters' entries)

Listed so the block diagram is fully accounted for;
these are **not** Cluster A entries and get their full
treatment in the media/connectivity inventories:
RGA 2D graphics engine (`rga@0xff980000`, SPI 87 —
graphics/display), RKIVE intelligent video engine,
VEPU H.264/H.265/JPEG encoder, ISP, VOP, MIPI CSI —
media. DMAC's two sibling controllers counted by the
datasheet are presumed to live inside those media
subsystems (§22). POR appears in the diagram as part of
the reset infrastructure and is covered under §21
(NPOR). The audio codec, SARADC, and all pin/routed
peripherals belong to the connectivity/pin clusters.

---

## Summary table — Cluster A

| # | Block | Exposure (Mini B) | QaonicOS status | Leverage |
|---|-------|-------------------|-----------------|----------|
| 1 | Cortex-A7 + VFP/NEON | the SoC | used (FP off pending THR-1) | medium |
| 2 | Caches / MMU | on-die | used | low |
| 3 | GIC-400 + IRQ map | on-die | partial — bound to QEMU addresses | high |
| 4 | TrustZone fabric | on-die | unused | low |
| 5 | Debug / JTAG | muxed balls, no header found | unused | low |
| 6 | HPMCU (RISC-V) | on-die | blocked — vendor TB firmware, unexamined | low |
| 7 | LPMCU (RISC-V) | on-die (VD_PMU) | unused (PWR-3 question) | low |
| 8 | Mailbox ×2 | on-die | unused | low |
| 9 | System SRAM 256 KB + PMU SRAM 8 KB | on-die | unused | low–medium |
| 10 | NPU 0.5 TOPS | on-die | unused, no register doc held | low |
| 11 | Crypto engine (v3) | on-die | unused — software crypto instead | high |
| 12 | TRNG | on-die | unused — weak xorshift instead | high |
| 13 | OTP 8 Kbit | on-die | unused, unread | medium |
| 14 | BootROM 20 KB | on-die | used (implicit, B0) | low |
| 15 | Watchdogs ×2 (+PMU_WDT) | on-die | unused — FND-3 is software-only | high |
| 16 | Timers (arch / RK block / HPTIMER) | on-die | arch timer used; RK block + HPTIMER unused | medium |
| 17 | RTC | on-die (VD_LOGIC) | unused | medium |
| 18 | TSADC | on-die | unused | medium |
| 19 | PVTM ×2 + PVTPLL | on-die | unused | low |
| 20 | GRF | on-die | unused | low |
| 21 | CRU (4 PLL, gating, resets) | on-die | unused on HW | high |
| 22 | DMAC (PL330) | on-die | partial — driver at real base, HW-gated | medium |
| 23 | DECOM (gzip/deflate) | on-die | unused | medium |

## Open questions

1. **Crypto-v3 register map.** The local U-Boot tree has
   v1/v2 only. Where does the project obtain the v3 map
   — the RV1106 TRM (not held) or the Linux rkcrypto
   driver (not checked out)? This gates §11's driver.
2. **HPMCU SRAM address discrepancy.** TOS ini loads the
   HPMCU image at `0x210f6000` [TR]; the DTS SRAM window
   is `0xff6c0000` with the HPMCU partition at offset
   0x3e000 [SDK]. Alias, MCU-local view, or error —
   resolvable only with the TRM memory map or hardware
   probing (folds into THR-4).
3. **The second RNG engine.** Datasheet: two engines
   (one secure-only). DTS: one `trngv1` node. Is the
   secure engine a separate block, and does its
   existence change what the NS TRNG can be trusted
   with?
4. **Rockchip timer block base + HPTIMER base.** Both
   blocks provably exist (datasheet; AMP clocks; TRM
   wake table) but neither has a node in the local DTS.
   TRM required before §16 can move.
5. **Secure WDT and PMU_WDT register locations.** Only
   the NS dw-wdt is DTS-described. The PMU_WDT's map is
   needed if sleep-state supervision is ever designed.
6. **WDT clock frequency.** Timeout-in-seconds for §15
   needs the actual `TCLK_WDT_NS` rate from the CRU
   assignment; only the 32-bit width is known locally.
7. **HPMCU clock.** The "400 MHz" figure in older
   project material traces to secondary web sources;
   the V1.4 datasheet does not state an MCU clock
   [TR]. Still unverified. (Carried over, not new.)
8. **Does the production sleep path need LPMCU
   firmware?** (Power research Q2, restated because
   §7 depends on it.) No rv1103b PM blob in rkbin hints
   FSM-only; unproven.
9. **JTAG exposure on the Mini B.** The balls exist as
   mux options shared with SDMMC/UART2/UART5; whether
   any test point or header pad reaches them on this
   board is not determinable from the schematic text
   extraction.
