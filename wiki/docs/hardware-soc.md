# Hardware & SoC — RV1103 / Luckfox Pico Mini B

The Pico Mini B pairs a Rockchip RV1103 SoC with 64 MB of SiP DDR2
and 128 MB of SPI NAND. The exhaustive inventory (56 blocks,
evidence-tagged per block) is
[`docs/riset-soc-rv1103/`](../docs/riset-soc-rv1103/RINGKASAN.md);
this page is the digest, **including the corrections** that research
made to older project assumptions.

> **Key facts (corrected)**
>
> | | |
> |---|---|
> | CPU | Cortex-A7 single-core @ **1.104 GHz** per vendor DTS — the "1.2 GHz" in older docs/strings is Luckfox marketing |
> | RAM / storage | 64 MB DDR2 (SiP) / 128 MB SPI NAND (Winbond W25N01GV) |
> | microSD | **The Mini B HAS a populated, wired TF socket** (SDMMC + DET pin) — the B0-era "no slot" conclusion was wrong; physical confirmation still pending |
> | Camera | **CAM FFC connector (H1) is routed**: one 2-lane MIPI CSI port; matching sensor = SC3336 3 MP; one camera max |
> | Display | **No VOP, no video decoder in silicon** — SPI → ST7789 is the only display path, permanently; no hardware video playback, ever |
> | Ethernet | GMAC + PHY exist in silicon; signal pairs reach castellated holes P401, but there are **no magnetics/RJ45** — a bench path, not a product port |
> | USB | DWC3 USB 2.0 OTG on USB-C, device-first; already the MaskROM flashing path; QaonicOS has zero USB code today |
> | Analogue audio | Codec MIC/LINEOUT nets are **not broken out** to any connector; the AUD_DSM speaker output sits on H2 pins GP10/GP11 (H4 must resolve physical attach) |

## Capability matrix (digest)

Exposure: **R** routed on the Mini B · **D** on-die only · **—** absent.

| Block | Exp. | QaonicOS status | Leverage |
|---|---|---|---|
| Cortex-A7 + VFP/NEON | D | used; FP codegen off until THR-1 (save/restore are empty stubs today) | med |
| GIC-400 + IRQ map | D | partial — bindings still QEMU-shaped | high |
| Crypto engine v3 (AES-GCM/XTS, SHA-256/HMAC, PKA 4096-bit) | D | **unused** — all crypto is software | **high** |
| TRNG `0xff448000` | D | **unused** — timer+xorshift entropy instead | **high** |
| Watchdogs ×2 + PMU_WDT (NS `dw-wdt` @ `0xff5a0000`, two-stage) | D | unused — FND-3 supervisor is software; HW WDT folds into its design | **high** |
| CRU (4 PLL, clock gating, resets) | D | unused on HW — the universal enabler every driver needs first | **high** |
| PL330 DMAC | D | partial — stage 1 (audio RX), structure-verified | high (enabler) |
| RGA2 2D engine `0xff980000` | D | **unused** — fill/blit/alpha/scale/rotate/CSC at a vendor-quoted ~2000 Mpix/s | **high** |
| MIPI CSI-2 + VICAP / ISP 4MP@30 | R (one port) | unused | high *with* camera |
| VEPU H.264/H.265 + JPEG encoder, RKIVE vision units | D | unused | med (high with camera) |
| Video decoder | — | **absent in silicon** | none |
| VOP display controller | — | **no output pins (QFN88)** | none |
| SPI0 → ST7789 | R | draft driver (the real display path) | planned |
| SFC / SPI NAND | R, populated | draft driver (most complete of the drafts) | high |
| SDMMC | R — populated TF socket | unused; FAT32 seam ready | high |
| USB 2.0 OTG (DWC3) | R — USB-C, device-first | unused (no stack) | high (device) |
| GMAC + FE PHY | pairs at P401, no magnetics | unused (DTS disabled) | med-high (dev) |
| SARADC | IN0 reserved (RECOVERY key), **IN1 free** | unused — battery sensing design in [Power & Battery](power-battery.md) | med |
| OTP 8 Kbit | D | unused, unread | med |
| RTC `0xff1c0000` | D | unused (DTS-evidenced; datasheet never mentions it) | med |
| TSADC (thermal) | D | unused — its automatic reset threshold is a candidate last-resort thermal guard | med |
| Timers (arch / RK block / HPTIMER) | D | arch timer only | med |
| Mailbox ×2 (`0xff5c0000` + PMU `0xff378000`) | D | unused | low |
| HPMCU (RISC-V camera core) | D | blocked — vendor test-board firmware may already load in the B0 boot chain; ground truth = THR-4 | low |
| LPMCU (RISC-V, PMU domain) | D | unused — open question inside PWR-3 (FSM-only sleep vs firmware) | low |
| NPU 0.5 TOPS | D | unused — **rejected**: closed RKNN stack, no register documentation, no QEMU model, no workload | low |
| System SRAM 256 KB + PMU SRAM 8 KB | D | unused | low–med |
| DECOM (gzip/deflate) | D | unused | low–med |
| GPIO (bank 2 deleted) | R (header subset) | partial — bank-0 set/get syscalls 40/41 | high |
| UART2/3/4 | R (UART5 not routed) | console PL011 only today; UART4 = the C6 link on hardware | high |
| PWM ×11–12 | R (subset) | unused — backlight dimming is the cheap certain win | med |

## Highest-leverage unused capabilities (exploitation order)

1. **CRU + GRF/IOC helpers** — every hardware driver (audio
   included) starts with a clock ungate + reset release QaonicOS has
   never performed as a shared service. Gates almost everything below.
2. **TRNG → `SYS_RANDOM`** — small driver, local U-Boot reference;
   retires the documented weak entropy (TLS timer+xorshift,
   counter-derived credstore nonces, xorshift-seeded setup secret).
3. **Hardware watchdog** — two-stage (interrupt first, reset on a
   second uncleared timeout) maps directly onto bootstate's
   reserved reason `BSR_WATCHDOG=3`; covers the kernel-itself-wedged
   case no software supervisor can.
4. **Crypto engine v3** — maps point-for-point onto shipped software
   (AES-GCM store, update SHA-256, RSA-2048 verify). Gated on
   procuring the v3 register map (TRM / Linux rkcrypto — not held locally).
5. **RGA2** — offloads face/uiapp composition (tens of µs per
   240×240 frame). Sequenced **after** the SPI display works; callers
   keep software fallbacks, and the QR screen stays on CPU (RGA
   scaling is filtered-only — a blurred QR is a broken QR).
6. **SDMMC driver** — turns the TF socket into storage expansion +
   a recovery channel; comes with a policy decision (the BootROM's
   SD-over-NAND boot priority is both a recovery feature and a
   provisioning hazard).
7. **USB device gadget** — the wired provisioning / update-delivery /
   recovery channel that de-risks several other tracks; the largest
   single driver in the inventory.
8. **Camera pipeline** (CSI-2 + ISP, then VEPU) — the product's only
   path to eyes; a subsystem-sized cost, gated on an explicit
   product decision (SOC-5).

## Key register addresses

From [`docs/hw-addrs.md`](../docs/hw-addrs.md) and the SoC
inventory's complete 65-node table
([`BAGIAN-C-IO.md`](../docs/riset-soc-rv1103/BAGIAN-C-IO.md)):

| Block | Base |
|---|---|
| GIC distributor / CPU interface | `0xff1f1000` / `0xff1f2000` |
| GRF | `0xff000000` |
| PMU | `0xff300000` |
| CRU | `0xff3a0000` |
| GPIO0 | `0xff380000` |
| TRNG | `0xff448000` |
| Crypto engine v3 | `0xff440000` |
| ACODEC | `0xff480000` |
| UART2 (debug console) | `0xff4c0000` |
| SPI0 | `0xff500000` |
| NS watchdog (dw-wdt) | `0xff5a0000` |
| SDMMC | `0xffaa0000` |
| SFC (SPI NAND controller) | `0xffac0000` |
| RGA2 | `0xff980000` |
| RTC | `0xff1c0000` |

Sources:
[`docs/riset-soc-rv1103/RINGKASAN.md`](../docs/riset-soc-rv1103/RINGKASAN.md)
(synthesis, matrix, corrections register §3),
[`docs/riset-soc-rv1103/BAGIAN-A-COMPUTE.md`](../docs/riset-soc-rv1103/BAGIAN-A-COMPUTE.md),
[`BAGIAN-B-MEDIA.md`](../docs/riset-soc-rv1103/BAGIAN-B-MEDIA.md),
[`BAGIAN-C-IO.md`](../docs/riset-soc-rv1103/BAGIAN-C-IO.md),
[`docs/hw-addrs.md`](../docs/hw-addrs.md).
