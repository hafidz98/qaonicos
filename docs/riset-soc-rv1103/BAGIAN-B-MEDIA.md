# RV1103 SoC Capability Inventory — Part B: Media, Display, Audio

Exhaustive inventory of the RV1103's media/display/audio blocks and what
QaonicOS can (and cannot) do with each on the Luckfox Pico Mini B.
Companion parts cover the other clusters (compute/NPU, connectivity,
system/security). Every block in the datasheet's multimedia complex gets
an entry, including the ones whose verdict is "absent" or "unusable".

Sources and evidence levels used throughout:

- **[DS]** *Rockchip RV1103 Datasheet V1.4* (2023-07-25, 25 pp.) —
  `~/workspace/luckfox-pico-mini-b/documents/Rockchip-RV1103-Datasheet-V1.4-20230725.pdf`.
  Brief datasheet: feature lists (§1.2), block diagram (Fig. 1-1, p. 13),
  pin list (Table 2-1, p. 19). It contains **no register maps**.
- **[DTS]** `rv1106.dtsi` in the Luckfox SDK U-Boot tree —
  `~/workspace/b0-sdk/luckfox-pico/sysdrv/source/uboot/u-boot/arch/arm/dts/rv1106.dtsi`.
  RV1103 inherits this file (established in `docs/hw-addrs.md`), so its
  node addresses/IRQ/clock names are the best local register-level
  evidence. Register *bit layouts* live in the RV1106 TRM media chapters
  and the vendor kernel drivers, **neither of which is in the workspace**.
- **[SCH]** Board schematic —
  `~/workspace/luckfox-pico-mini-b/documents/schematic.pdf` (Luckfox Pico
  family sheet; connector H1 is literally labelled "CAM", header H2 is
  the "Luckfox Pico" GPIO header). Pin endpoints below were traced from
  the schematic's net labels, not assumed.
- **[CODE]** QaonicOS source / docs as cited per entry.
- **[WEB]** Vendor/community web sources, used only where local sources
  are silent (RGA documentation ecosystem); always marked.
- **[INF]** Inference from the above; **[UNK]** unknown.

A note on method: the RV1103 is a *camera* SoC. Its multimedia complex
faces **inward** (sensors → ISP → encoder). Its display story is the
opposite of a phone SoC's, and §7–§8 are where that lands.

---

## 1. MIPI CSI-2 video input (2× DPHY + 2× CSI-2 hosts + VICAP)

**What it does** — Receives raw sensor video. Two MIPI D-PHYs, DPHY
v1.2, 2 lanes each at up to 1.5 Gbps/lane; the two PHYs can combine
into one 4-lane port. The same combo IO also receives LVDS (sub-LVDS
sensor) signalling. Two CSI-2 host controllers feed the VICAP capture
block, which DMA-writes frames to DDR and hands them to the ISP.

Registers/driver: **[DTS]** `mipi0_csi2@ffa20000`, `mipi1_csi2@ffa30000`
(compatible `rockchip,rk3588-mipi-csi2`, IRQ 99/100 and 101/102);
PHY hardware block `csi2-dphy-hw@ff3e8000`, presented as three logical
PHYs (dphy0 = full mode, dphy1/dphy2 = split modes); capture block
`rkcif@ffa10000` (compatible `rockchip,rv1106-cif`, IRQ 106, twelve
VICAP clocks incl. per-input `i0clk/i1clk`, `rx0/rx1clk`). Sensor clock
outputs `MIPI_CLK0_OUT`/`MIPI_CLK1_OUT` (package balls 1/3) **[DS]**.
Full register programming = TRM + the vendor `rkcif`/`csi2-dphy`
kernel drivers (not local).

Pico Mini B exposure: **routed — one port.** Schematic **[SCH]**:
connector **H1, labelled "CAM", footprint "FFC 0.5 20P"** (20-pin
0.5 mm FFC, staggered vertical SMD). Its pins: 3 = MCLK0, 4/5 = D0P/D0N,
7/8 = CLK_P/CLK_N, 10/11 = D1P/D1N, 13/14 = sensor I2C SCL/SDA,
16 = RST, 18–20 = VCC_3V3, rest GND. So **one 2-lane CSI port
(DPHY0) reaches a real connector**, with sensor clock, control I2C
and reset. The second PHY's nets (D2/D3 + CLK1) appear in the
schematic only as labels at the SoC — no second connector exists.
The sensor Luckfox pairs with this connector is the SC3336 3 MP
module (its datasheet ships in the reference clone,
`documents/SC3336-3MP-Camera-B-*.pdf`) **[SCH/vendor]**.

QaonicOS status: **unused.** No CSI/VICAP code or addresses anywhere
in the tree (grep: zero hits outside this programme's docs).

Abstraction it implies: a kernel capture service (new syscalls:
configure sensor over its I2C, program DPHY + VICAP, ring of DDR frame
buffers, frame-ready event into the existing event queue) — the
camera twin of the audio-capture path (syscalls 84–88).

Leverage: **high** (as a pipeline with §2/§3 — it is the product's
only path to eyes), but it is a subsystem, not a driver.

## 2. ISP (image signal processor)

**What it does** — The full Bayer pipeline: BLC, DPCC, PDAF, LSC,
Bayer 2DNR/3DNR, CAC, HDR merge (2-frame) + DRC tone mapping, GIC,
advanced DeBayer, CCM/CSM, gamma, dehaze/enhance, 3D-LUT, LDCH,
YUV 2DNR, sharpening, privacy mask (CMSK), local gain; 3A statistics
(AE/AF/AWB) out. Max input 4 Mpixel @ 30 fps, min 256×256; one ISP
time-multiplexes up to 4 sensors; "MI 3+2 path" outputs with
stepless main-path scaling **[DS §1.2.9]**.

Registers/driver: **[DTS]** `rkisp@ffa00000` (0x7F00, compatible
`rockchip,rv1106-rkisp`, IRQs 105 isp / 104 mi / 103 mipi; clocks
`ACLK_ISP3P2`/`CLK_CORE_ISP3P2` — the "ISP3P2" clock names mark this
as Rockchip's ISP 3.2-generation pipeline **[INF]**). Real use needs
the vendor's RKAIQ tuning stack (3A algorithms + per-sensor tuning
files) — a large closed userspace component in the SDK **[WEB/INF]**.

Pico Mini B exposure: reachable exactly as far as §1 reaches — one
sensor on the CAM connector. No sensor, no ISP input.

QaonicOS status: **unused** (nothing).

Abstraction it implies: none sane without §1 and a sensor decision;
if the camera lands, the ISP is configured as part of the capture
service, not exposed raw. RKAIQ would have to be ported or replaced
with fixed tuning — the single biggest software cost in the vision
pipeline **[INF]**.

Leverage: **high with a camera, zero without.** It converts a raw
sensor into usable frames; it has no other consumer.

## 3. Video encoder (VEPU / rkvenc) — H.264 / H.265 / DVBM

**What it does** — Hardware video encoding: H.265/HEVC Main Profile
L5.0 and H.264/AVC High Profile L5.0, multi-stream, total throughput
4 Mpixel @ 30 fps; bitrate to 60 Mbps; six rate-control modes (CBR,
VBR, FIXQP, AVBR, QPMAP, CVBR); YUV420/YUV400 input; crop/rotate/
mirror of the source; 8-region OSD overlay; ultra-low-delay mode
**[DS §1.2.4]**. A companion block, **DVBM** (dedicated video buffer
manager), manages the encoder's reference-frame buffers in DDR.

Registers/driver: **[DTS]** `rkvenc@ffa50000` (0x6000, compatible
`rockchip,rkv-encoder-rv1106`, IRQ 107; core clock assigned
400 MHz, ACLK 300 MHz); `rkdvbm@ffa70000` (IRQ 116). Driven in the
SDK by Rockchip MPP (`rockchip,srv = <&mpp_srv>`) — the register
protocol is a task-queue/command-buffer interface, documented by
the MPP source and TRM, not by anything local **[DTS/WEB]**.

Pico Mini B exposure: on-chip only; needs nothing from the board
except DDR (in-package) and an input source (§1/§2, or synthetic
frames from DDR).

QaonicOS status: **unused.**

Abstraction it implies: an encode service behind the capture service
(frame handle in → bitstream chunk out), or a standalone
`SYS_VENC_*` family if a camera never lands (e.g. encoding the
rendered UI for remote view — technically possible, low value).

Leverage: **medium** standalone, **high as the back half of the
camera pipeline** (record/stream what the ISP produces; also the
only way a future "send a clip to the phone" feature is affordable).

## 4. JPEG encoder

**What it does** — JPEG baseline encoding, up to 4 Mpixel @ 60 fps in
standalone mode, max resolution 8192×8192 **[DS §1.2.4]**.

Registers/driver: no separate DTS node — on this generation JPEG is
a mode of the VEPU/rkvenc core (§3), driven through the same MPP
task interface **[DTS/INF]**.

Pico Mini B exposure: on-chip only.

QaonicOS status: **unused.** (The face's QR screen and UI are
composited as raw RGB565; nothing encodes images today.)

Abstraction it implies: rides §3's encode service
(`encode_jpeg(frame) → bytes`), no separate ABI.

Leverage: **medium** — snapshot-to-phone / snapshot-to-C6 is the
cheapest camera feature there is, and JPEG of a *rendered* frame is
also the natural "screenshot share" primitive. Still gated on §1
for real photos.

## 5. Video decoder — ABSENT

**What it does** — Nothing: the RV1103 has **no hardware video
decoder**. The datasheet's Video CODEC section (§1.2.4) describes an
encoder only; the block diagram (Fig. 1-1) shows `VEPU` and
`JPEG Encoder` and no decode block; the shared DTS contains no
vdpu/vdec node. Secondary comparison tables likewise leave
RV1103 "video decoding" blank **[DS/DTS]**.

Pico Mini B exposure: n/a. QaonicOS status: n/a — and this is a
*product* constraint worth stating plainly: **the device can never
play H.264/H.265 video in hardware.** Any future "show a clip"
feature is MJPEG/JPEG-sequence or software decode at tiny
resolutions, or nothing.

Abstraction it implies: none. Leverage: **none (absent)** — listed
so the inventory is closed.

## 6. RKIVE — intelligent video engine (RVE)

**What it does** — A fixed-function vision-accelerator complex with
22 calculation units **[DS §1.1, §1.2.6]**: background modelling
(GMM 1–5 models, codebook BGM), Canny, connected components (≤254
regions), corner detect (≤500, sorted), Lucas–Kanade optical flow
(≤500 corners, 1–4 layers), integral image, LBP, 3×3/5×5 filter,
Sobel, morphology (erode/dilate), denoise (min/median/max), DMA copy,
colour-space conversion (RGB↔YUV↔HSV, 601/709), histogram /
equalised histogram, logic ops (and/or/xor/add/sub/absdiff),
gradient magnitude+angle, NCC template match, cast (linear remap),
SAD (4×4/8×8/16×16), threshold, LUT map (8→8, 8→16).

Registers/driver: **[DTS]** `rve@ffad0000` (0x1000, compatible
`rockchip,rve`, IRQ 111; clocks literally named `ACLK_IVE`/
`HCLK_IVE` — RVE is the register block behind the "IVE" feature
name). Programming model = per-unit register windows + DMA
descriptors in the vendor kernel `rve` driver / TRM (not local).

Pico Mini B exposure: on-chip only; consumes DDR frames, so it is
usable *without* a camera too (frames can come from software).

QaonicOS status: **unused.**

Abstraction it implies: a batch image-op syscall family
(`SYS_IVE_*`: submit op + src/dst buffers → completion event),
sitting next to §10's proposed RGA ABI; the two share the
buffer-addressing problem (see §10).

Leverage: **medium.** Motion detection (GMM + threshold + CCL on
downscaled frames) is the classic always-on IPC workload and maps
1:1 onto these units — that is a real Qabot feature ("notice when
someone is there") at near-zero CPU. Without a camera its value
collapses to CSC/filter chores that §10 does better.

## 7. RGA2 — 2D graphics engine ★ high interest

**What it does** — The SoC's only general-purpose pixel engine
**[DS §1.2.7]**: BitBLT (block copy, colour fill with alpha, colour
keying/stencil transparency, two-source blits), scaling with
arbitrary non-integer ratio 1/16–16 (average down-scale, bicubic
up-scale), rotation by 90° steps + mirror, per-pixel alpha blending
with fading, pixel-format conversion (ARGB/RGB888/RGB565/RGB4444/
RGB5551, YUV420/422 in and out, BT.601/709, dithering), OSD overlay
with automatic inversion (ARGB8888/1555/4444/2bpp OSD sources), and
rectangular mosaic. Max 8192×8192 source, 4096×4096 destination.

Registers/driver: **[DTS]** `rga2@ff980000` (0x1000, IRQ 87; clocks
`ACLK_RGA2E`/`HCLK_RGA2E`/`CLK_CORE_RGA2E` — the "E" marks the
RGA2-Enhance variant). **Documentation quality is the weak point,
stated honestly:** no register map in the datasheet, no driver in
the local SDK checkout (the kernel tree is absent from
`b0-sdk/luckfox-pico/sysdrv` — only U-Boot is present). The register
file is small and fully described *outside* the workspace: the
RV1106 TRM's RGA chapter (the same DeciHD `rockchip_docs` mirror
the audio track already cites for TRM V0.3), Rockchip's RGA
developer guide + FAQ (EN PDFs in that mirror), and the vendor
kernel `rga2` driver (GPL) with the Apache-2.0 `librga` userspace
IM2D API on top **[WEB]**. The vendor FAQ dumps the entire command
register set in a few dozen words — this is a days-scale register
port, not a reverse-engineering project **[WEB]**.
Memory model: RGA2 is a bus master over **physical DDR addresses**
(its own MMU translates page tables the driver builds; on a 64 MB,
identity-mapped system like ours, plain physical addressing or a
trivial page table suffices). Buffers must be physically locatable
and cache-coherent with the CPU — the two real integration costs
(see sub-assessment). Vendor performance anchor (RGA2-Enhance
@ 300 MHz, from the developer guide): ~2000 Mpixel/s copy,
~1600 Mpixel/s alpha blend at 1080p — a 240×240 frame is 0.058
Mpixel, i.e. **tens of microseconds per full-screen op** **[WEB]**.

Pico Mini B exposure: on-chip; board-independent. Reachable the
moment a driver exists.

QaonicOS status: **unused — and its absence is load-bearing.**
All rendering today is CPU loops: the face composes every frame
pixel-by-pixel into an RGB565 buffer (`user/face/face_draw.c`:
row-loop fills, bounding-box circle/squircle loops with per-pixel
distance tests), uiapp does the same (`user/uiapp/ui_draw.c`), and
the result is flushed in strips through `SYS_DISPLAY_FLUSH`
(syscall 61) — in QEMU to virtio-gpu, on hardware (planned) pushed
over SPI0 to the ST7789. The adopted LVGL plan
(`docs/riset-ui-framework/RINGKASAN.md`, UI-1..UI-6) assumes a
software renderer throughout; RGA appears nowhere in it **[CODE]**.

Abstraction it implies: a small kernel 2D service —
`SYS_RGA_FILL / SYS_RGA_BLIT / SYS_RGA_BLEND / SYS_RGA_SCALE`
over named DDR framebuffers, completion by IRQ 87 → event, plus a
framebuffer allocator story (below). LVGL would consume it through
a custom draw/flush backend, not by replacing LVGL's renderer.

Leverage: **high** — the only unused block in this cluster that
improves the product QaonicOS already ships (see sub-assessment).

### 7.1 Sub-assessment: today's rendering costs vs RGA ops

| Today's software cost | Where | RGA mapping |
|---|---|---|
| Full-screen clear + rect fills every frame | face `fb_fill_rect`, uiapp `ui_rect` | **Direct**: RGA colour fill. Removes the memset-class cost of every frame. |
| Squircle/eye shapes, circles (bounding-box loops + per-pixel distance tests) | face `face_draw.c` | **Partial**: RGA has no procedural shapes. Pre-render shapes once to sprites, then let RGA blit/blend them (blink/morph = sprite select + alpha fade). The per-pixel shading of the "thinking" sphere does **not** map — it stays CPU, or becomes a small sprite set cross-faded by RGA. |
| Waveform bars (per-sample 2 px rects) | face `fb_waveform` | **Direct-ish**: batched fills; or render bars to a small offscreen and blit. Either way the full-screen recomposition around them disappears. |
| Status bar + menu tiles + label compositing | uiapp screens | **Direct**: this is textbook 2-source blit + alpha blend — exactly what the OSD/blend path exists for. |
| Strip flushes (CPU-composed strips copied to the display path) | `SYS_DISPLAY_FLUSH` callers | **Changes shape, not existence**: on hardware the final 240×240 must still be pushed over SPI0 byte-by-byte (ST7789 has no other input — §8). RGA removes the *composition* cost before the push; it cannot remove the push. In QEMU it could blit straight into the virtio-gpu framebuffer. |
| Future LVGL composition (UI track) | planned | **Direct**: LVGL draws widgets into partial buffers in software by default; RGA accelerates the expensive parts (fills, blends, image scaling/rotation) via a custom backend, and panel rotation — today a MADCTL trick or CPU transpose — becomes an RGA rotate blit. |
| QR setup screen (integer ×3/×4 nearest upscale of a 1-bit code) | face `qr.c` + draw | **Keep on CPU**: RGA up-scaling is bicubic/bilinear only — filtered scaling can make a marginal QR unreadable. Blit (1:1 copy) is safe; scaling is not. Recorded so nobody "optimises" this later. |
| Camera overlay / face-on-video (future) | — | **Direct**: YUV frame + RGB565 UI overlay + blend + CSC is RGA's home turf (it even has a dedicated SRC(YUV)+SRC(RGB)→DST path in the blend unit **[DS]**). |

What stands in the way, in order:

1. **No QEMU model.** QEMU's `virt` has no RGA; every line of an
   RGA driver is hardware-tested only, breaking the project's
   QEMU-first verification rule (docs/VERIFIKASI-GUI.md). The
   driver must therefore be small, and its *callers* need a
   software fallback path that QEMU can verify (renderers already
   exist — RGA becomes an accelerator behind the same API, not a
   new API). This, more than register risk, is the real cost.
2. **Buffer addressing + coherency.** RGA eats physical addresses
   and does its own DMA; QaonicOS today has no user-visible
   contiguous-framebuffer allocator, and the A7's D-cache must be
   cleaned/invalidated around RGA jobs. Solvable (the kernel owns
   the display pipeline already), but it is new kernel machinery.
3. **Register documentation is external.** TRM chapter + vendor
   driver must be pulled in (both identified above); nothing
   blocks this, it just hasn't been done.
4. **Clock/reset plumbing.** `ACLK_RGA2E` et al. need CRU/GRF
   enable code of the same class as the audio driver's — known
   work, no surprises expected **[INF]**.
5. **Honest scale check.** At 240×240 the CPU *can* do this job —
   it does today. RGA's win is not feasibility; it is CPU time and
   power handed back to the agent loop (and, with a camera,
   overlay composition). That argues for sequencing RGA **after**
   the SPI display works end-to-end, not before.

## 8. Display output — VOP (present on paper, dead on this package) and the real SPI path

**VOP** — A display controller node exists in the shared DTS:
`vop@ff990000` (0x200, compatible `rockchip,rv1106-vop`, IRQ 89),
with exactly one output port, `vop_out_rgb` → an RGB (parallel
TTL) interface; a `display-subsystem` node wraps it **[DTS]**.
On the **RV1103** this is a dead end, on three independent pieces
of evidence: (1) the RV1103 datasheet's feature list has **no
display section at all** (grep: "display", "VOP", "DSI", "LCDC"
occur zero times); (2) the QFN88 pin list (Table 2-1) contains
**no RGB/LCDC/DSI pins** — the balls that carry display signals on
the QFN128 RV1106 simply do not exist here; (3) secondary
comparisons list display interfaces for RV1106 (RGB666,
1280×720 max) and leave RV1103's cell empty **[DS/DTS/board
README]**. Conclusion: VOP may exist on the shared die, but on
RV1103 + Mini B it has **no output path and can never drive
anything**. There is likewise **no MIPI DSI, no HDMI, no BT.1120
output** on this chip/package.

**The real display path is SPI.** The product's panel story —
unchanged by anything in this inventory — is: CPU (and later §7)
compose into a DDR framebuffer → push over **SPI0**
(`spi@ff500000` **[DTS]**) to an ST7789 240×240 panel. QaonicOS
has compile-checked drafts for exactly this
(`kernel/kernel/arm/draft/spi0.c`, `st7789.h`, `spi_lcd.h`; SPI0
pins + GPIO DC/RST/BL are TODO(HW) there) and the clone's
`spi/fb7789.py` proves the same path from Linux userspace
**[CODE/SDK]**. Backlight drive belongs to §9 (PWM), not to a
display controller.

Abstraction: the existing display-token + `SYS_DISPLAY_FLUSH`
model (syscalls 60–68) already *is* the abstraction; nothing new
is implied — only the SPI backend behind it (H1.3 in the hardware
test plan).

Leverage: **none for VOP (unusable)**; the SPI path is already
planned work, listed here to close the "display controller"
question definitively.

## 9. PWM array

**What it does** — 11 on-chip PWM channels per the datasheet
(PWM0–PWM6, PWM8–PWM11), each with a 32-bit counter, prescaler,
continuous/one-shot modes, capture mode, and interrupt; PWM3 and
PWM11 are "optimised for IR" (IR remote waveform generation)
**[DS §1.2.3]**. The shared DTS defines twelve register blocks —
pwm0–3 @ `0xFF350000` (0x10 stride), pwm4–7 @ `0xFF360000`,
pwm8–11 @ `0xFF490000`, compatible `rockchip,rv1106-pwm` /
`rk3328-pwm` — including a pwm7 the datasheet's count omits (ball 3
muxes `PWM7_IR_M2`); treat the exact channel census as 11–12,
discrepancy noted in Open Questions **[DTS/DS]**.

Pico Mini B exposure: **routed, generously.** PWM muxes live on
the GPIO1_C/D balls that the H2 user header exposes (e.g. PWM2/4/
5/6 on GPIO1_C0–C3, PWM8/9 on C4/C5, PWM10 on C6 and D1) **[DS
Table 2-1 + SCH H2]**. The clone's working example drives
`/sys/class/pwm/pwmchip10` (i.e. PWM10) from Linux **[SDK]**.

QaonicOS status: **unused** — no PWM driver; the ST7789 draft
plans GPIO-driven backlight (on/off only).

Abstraction it implies: `SYS_PWM_CONFIG / SYS_PWM_SET` (channel,
period, duty) owned by the kernel's pin-mux layer; first consumer
is brightness (face power FSM: dim before display-off), then
buzzer/haptics and IR blasting if ever wanted.

Leverage: **medium** — the cheapest certain win in this cluster:
real backlight dimming (a visible product-quality and power
feature) for one small, well-documented, register-simple driver.

## 10. ACODEC (audio codec) — status only

**What it does** — Integrated stereo codec: two 24-bit ADC
channels (90 dB SNR, differential or single-ended mic/line in,
mic bias) and one 24-bit DAC channel with stereo playback;
sample rates 8–96 kHz **[DS §1.2.10]**; analog pins `CODEC_MIC0P/
MIC1P/MICN`, `CODEC_MICBIAS`, `CODEC_LINEOUT`, `CODEC_VCM`
(balls 21–26) **[DS]**.

Registers/driver: base `0xFF480000`; register semantics from
TRM V0.3 Ch. 21 and the shipping `rv1106_codec.{c,h}` — all
established in `docs/riset-audio-rv1103/RINGKASAN.md`, not
repeated here.

Pico Mini B exposure: **the codec's analogue nets appear in the
schematic only at the SoC symbol** — no connector, header or
mic/speaker footprint carries them in the sheet text **[SCH]**.
Whether physical pads exist is **[UNK]**; as drawn, board-level
analogue audio I/O is not broken out (the H4 hardware test will
have to resolve how a microphone is actually attached — this
inventory only records that the schematic does not show one).

QaonicOS status: **used (QEMU-verified)** — driver D1–D4 done
(ACODEC init, gain, capture path; `docs/rencana-driver-audio/`);
hardware bring-up is H4, pending the exposure question above.

Abstraction: exists (syscalls 82–88 family). Leverage: n/a here —
see the audio research.

## 11. I2S0 / TDM controller + AUD_DSM output — status only

**What it does** — The codec's digital audio link: `i2s0_8ch`
(multichannel TDM-capable I2S) carries ADC samples codec → I2S →
PL330 DMA, and DAC samples back **[DTS: `i2s@ffae0000`,
`rockchip,rv1106-i2s-tdm`]**. There are **no external I2S pins**
on the QFN88 (Table 2-1 has no I2S signals) — this controller is
an internal bridge, not a user audio port **[DS]**. A second
output flavour exists: `AUD_DSM_P/N` (balls 54/55), a delta-sigma
modulated output for direct (class-D style) speaker drive, muxed
onto GPIO1_D3/D2 — which *are* on the H2 header **[DS/SCH]**.

QaonicOS status: **used (QEMU-verified)** — D2/D3 implemented
PIO capture + DMA stage 1 (`docs/rencana-driver-audio/`); the
DSM output is **unused** and previously unremarked.

Abstraction: exists for I2S (syscalls 84–92 family incl. PL330).
DSM would ride the same playback path with a different output
mux — a hardware-phase decision (H4), not new ABI.

Leverage: **low** (DSM is an output-routing alternative, noted
for completeness — it may be the *only* speaker path this board
physically exposes, which H4 should check before assuming
LINEOUT goes anywhere).

## 12. PDM microphone input — ABSENT

No PDM block exists: "PDM" occurs zero times in the datasheet,
and there is no PDM node in the DTS **[DS/DTS]**. Digital-mic
input, if ever wanted, would have to arrive as I2S from an
external codec — which §11 shows has no external pins. Voice
input on this SoC means the analogue codec (§10), full stop.

Leverage: **none (absent)** — listed to close the question.

---

## Summary table

| # | Block | Pico Mini B exposure | QaonicOS status | Leverage |
|---|---|---|---|---|
| 1 | MIPI CSI-2 ×2 + VICAP | **One 2-lane port on CAM FFC (H1)**; 2nd PHY unrouted | unused | high (with §2/§3) |
| 2 | ISP (4MP@30) | via §1 only | unused | high with camera, else zero |
| 3 | VEPU H.264/H.265 encoder + DVBM | on-chip | unused | med (high with camera) |
| 4 | JPEG encoder (in VEPU) | on-chip | unused | medium |
| 5 | Video decoder | — | **absent in silicon** | none |
| 6 | RKIVE / RVE (22 vision units) | on-chip | unused | medium |
| 7 | **RGA2 (2D engine)** | on-chip | unused | **high** |
| 8 | VOP display controller | **no output pins on RV1103** | unusable | none |
| 8b | SPI0 → ST7789 (the real display path) | SPI0 on header/castellated pins | draft driver, planned | (already planned) |
| 9 | PWM ×11–12 | several channels on H2 header | unused | medium |
| 10 | ACODEC | analogue nets not broken out **[SCH]** | **used** (D1–D4, QEMU) | — (audio research) |
| 11 | I2S0/TDM + AUD_DSM | internal only; DSM pins on H2 | used (I2S); DSM unused | low |
| 12 | PDM input | — | **absent in silicon** | none |

Noted for the other clusters (in the same block diagram, not
scored here): NPU 0.5 TOPS (compute cluster), DECOM gzip/deflate
engine and the crypto engine (system cluster), MAC+PHY (connectivity
cluster).

## Open questions

1. **RGA2 register bring-up source of truth**: pull the RV1106 TRM
   RGA chapter + vendor `rga2` driver into the workspace (as was
   done for the codec) before any RGA phase is planned; confirm the
   RGA2-Enhance variant's exact op set against the TRM (the
   datasheet list may include RGA3-isms).
2. **QEMU strategy for RGA/ISP/VEPU**: none of these blocks has a
   QEMU model. Do we accept hardware-only verification for their
   drivers (with QEMU-verified software fallbacks), or build a
   register-level stub device in our QEMU harness first?
3. **Camera sensor decision**: is the SC3336 module (Luckfox's
   standard for this connector) the target sensor? Its tuning
   files / RKAIQ expectations decide whether §2 is a port or a
   research project of its own.
4. **Analogue audio exposure**: the schematic shows CODEC_MIC/
   LINEOUT nets only at the SoC. Before H4, confirm on the real
   board (continuity/pads) how a microphone and speaker are meant
   to attach — AUD_DSM on H2 pins GP10/GP11 may be the intended
   speaker path.
5. **PWM channel census**: datasheet counts 11 (no PWM7) while
   the pin list and DTS both contain a PWM7 (IR variant, ball 3 /
   `0xFF360030`). Which channels actually exist on RV1103 silicon?
6. **Second DPHY**: D2/D3+CLK1 nets exist at the SoC but reach no
   connector on this board — confirm they are simply unrouted
   (not, e.g., tied to the Ethernet castellations) before the
   inventory's "one camera max" claim is treated as final.
7. **VOP on-die**: does RV1103 silicon physically contain the VOP
   the shared DTS describes (making its clock/power domain a
   keep-off concern), or is the node pure RV1106 inheritance?
   Affects only power hygiene, not features.
8. **DVBM memory carve-out**: the encoder wants a dedicated
   buffer-management scheme in a 64 MB system; sizing it is part
   of any future VEPU phase, not decidable from datasheets.
