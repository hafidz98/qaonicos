# RV1103 SoC Capability Inventory — Part C: I/O, Interconnect, Memory

Date: 2026-10-23. RESEARCH ONLY — no code was changed. This is
Part C (I/O, interconnect, memory) of the exhaustive RV1103
capability inventory for QaonicOS on the Luckfox Pico Mini B.
Audio registers are owned by `docs/riset-audio-rv1103/` and
power states / the PMU by `docs/riset-power-battery/`; both are
cross-referenced, not redone. Vision/media blocks (ISP, CIF,
CSI, RKVENC, RGA, RKIVE, NPU) belong to Part A; they appear
here only in the complete address table, for
`docs/hw-addrs.md` completeness.

Method: datasheet feature chapter + pin list read in full;
every `reg` node of the vendor device tree
(`hw/dts/rv1106.dtsi`, inherited by `rv1103.dtsi`) extracted
mechanically; the Mini B board DTS
(`rv1103g-luckfox-pico-mini.dts`) and the board schematic
(`~/workspace/luckfox-pico-mini-b/documents/schematic.pdf`,
text layer) checked per block; the community clone
(`~/workspace/luckfox-pico-mini-b/`, with gpio/spi/adc/pwm
examples) used for board-level facts. Evidence levels are
marked **[DS]** datasheet V1.4, **[SDK]** vendor device
tree / shipping driver / SDK, **[SCH]** Mini B schematic,
**[code]** QaonicOS source as of this writing, **[inf]**
inference, **[?]** unknown from local sources.

A warning that shapes the whole document: the RV1103 device
tree is the RV1106 tree with deletions. Several blocks exist
as DTS nodes but have **no pins on the QFN88 RV1103 package**
(UART0/1, I2C1/2, SPI1, VOP) and one is deleted outright
(GPIO2). "In the DTS" never implies "on the board" — every
entry below separates die capability from package routing
from board routing.

## 0. Ground facts (chip vs package vs board)

- Die: single Cortex-A7 + RISC-V MCU, in-package DDR2
  (RV1103G1 = 512 Mbit = 64 MB SiP), QFN88 9×9 mm [DS §1.1,
  §2.1]. Almost every peripheral ball is shared by 2–4 mux
  functions, so the board — not the die — decides what can
  be used *simultaneously* [DS §2.6].
- The Mini B routes: SPI NAND (populated), a microSD socket
  (populated — §6), USB-C (device role), two 20-pin headers
  H1/H2, a 5-pin Ethernet-pair footprint P401 (no magnetics
  — §10), and the camera connector [SCH].
- QaonicOS today runs only under QEMU (virtio devices); on
  real hardware it currently uses **none** of the blocks in
  this inventory except through the boot chain (B0) — every
  "status" below is written against that reality [code].

---

## 1. GPIO

**What it does** — General-purpose I/O: five 32-pin banks
(160 lines on the die), each line usable as an interrupt
source (level or edge, configurable polarity, rising /
falling / both edges), with per-pin weak pull-up/pull-down
and configurable drive strength [DS §1.2.12].

Registers/driver: Rockchip GPIO banks at GPIO0 `0xFF380000`
(SPI 5), GPIO1 `0xFF530000` (7), GPIO2 `0xFF540000` (9),
GPIO3 `0xFF550000` (11), GPIO4 `0xFF560000` (13), 0x100
windows [SDK: rv1106.dtsi; hw-addrs.md]. Pin mux/pull/drive
live in the IOC block (§20), GPIO0's in PMUIOC. Register
semantics from the shipping `gpio-rockchip.c` [SDK].

Pico Mini B exposure: routed, partially. Header H1/H2 carry
a subset (Luckfox documents 17 GPIO pins on the header
[SDK/clone README]); GPIO0 is a PMU-domain bank — of its
balls only A4 (pin 42, `PWR_CTRL`) leaves the package; the
C6 wake source GPIO0_INT is in this bank (power research).
**RV1103 quirk:** `rv1103.dtsi` contains
`/delete-node/ &gpio2;` — the vendor treats RV1103 as
having banks 0, 1, 3, 4, and no GPIO2-muxed ball appears in
the QFN88 pin list [SDK, DS §2.6]. Whether bank-2 hardware
exists on the die is unknown (open question Q8).

QaonicOS status: partial. `gpio.c` implements
`SYS_GPIO_SET` (40) / `SYS_GPIO_GET` with the **bank fixed
to 0** (user.c comment: "Bank di-fix 0") [code]; a shell
`gpio` builtin exists. Physical buttons (H1.2) and every
real pin consumer are hardware-phase; in QEMU the GUI is
driven by injected events (syscall 105).

Abstraction it implies: widen `SYS_GPIO_SET/GET` to
(bank, pin) — the syscall shape already takes a pin
argument — and add one new syscall for the missing half:
GPIO interrupt → kernel event queue (the EV_* path the GUI
already consumes), e.g. `SYS_GPIO_IRQ(pin, edge, enable)`.
Leverage: **high** — buttons, the C6 wake line, display
DC/RST/BL, LEDs and the RECOVERY-adjacent key handling all
hang off this one block; it is the cheapest entry in the
inventory to finish.

## 2. UART (×4 real: UART2–UART5)

**What it does** — Asynchronous serial: 64-byte TX and RX
FIFOs, 5–8 data bits, parity, and hardware auto flow
control (RTS/CTS) on all instances; with a suitable input
clock the baud rate reaches 4 Mbps [DS §1.2.11].

Registers/driver: six nodes in the inherited DTS —
UART0 `0xFF4A0000`, UART1 `0xFF4B0000`, UART2 `0xFF4C0000`,
UART3 `0xFF4D0000`, UART4 `0xFF4E0000`, UART5 `0xFF4F0000`
(SPI 25–30), compatible `snps,dw-apb-uart` (DesignWare APB
UART register map) [SDK]. The datasheet counts four UARTs
(UART2–UART5) for RV1103; UART0/UART1 have no balls on
QFN88 and are RV1106 phantoms [DS §1.2.11, §2.6].

Pico Mini B exposure: three routed (established by the
F5/C6 research, corrected 2026-10-19): **UART2 = debug
console** (M1 balls 49/50 shared with JTAG, M0 balls 9/10
shared with the SDMMC group; TX/RX reach header H2 [SCH]),
**UART4 = the ESP32-C6 link** (M1: TX ball 60, RX ball 61,
RTSN ball 52, CTSN ball 53 — full flow control available on
the header), **UART3 = spare** (M1: TX ball 57, RX ball
56). UART5 is **not routed** on the Mini (Pico Plus only)
[SDK board DTS marks uart3/uart4 present-but-disabled;
project docs `riset-esp32c6-companion` §7].

QaonicOS status: unused on hardware. The kernel console
driver (`uart.c`) targets the QEMU PL011; no Rockchip UART
driver exists yet, and no user syscall exposes a UART. The
C6 link plan (`docs/rencana-komunikasi-c6/PLAN.md`) names
the RV1103 UART4 driver as a hardware follow-up and
deliberately routes user access through a link daemon, not
a raw UART syscall.

Abstraction it implies: none new at the ABI level by
design — kernel-internal `uart4_*` driver under the link
layer (C6L); the debug console stays kernel-owned.
Leverage: **high** — UART4 is the C6 product link at
921600 baud (FIFO + PL330 DMA, §12, is the sane transport),
and UART2 is the only console the real device will ever
have.

## 3. I2C (×3 real: I2C0, I2C3, I2C4)

**What it does** — I2C masters: 7-bit and 10-bit
addressing, software-programmable clock, 100 kbit/s
standard / 400 kbit/s fast mode [DS §1.2.11].

Registers/driver: five DTS nodes — I2C0 `0xFF310000`,
I2C1 `0xFF320000`, I2C2 `0xFF450000`, I2C3 `0xFF460000`,
I2C4 `0xFF470000` (SPI 18–22), compatible
`rockchip,rv1106-i2c` [SDK]. The datasheet counts three
masters for RV1103 (I2C0, I2C3, I2C4); I2C1/I2C2 are
phantoms [DS §1.2.11].

Pico Mini B exposure: constrained by mux collisions
[DS §2.6]. I2C0 (M2) sits on balls 11/12 — the SDMMC
CLK/CMD balls — so it fights the TF socket (§6). I2C3 (M1)
is on balls 54/55, which also carry AUD_DSM and SPI0_CS1
options. I2C4 (M1) is on balls 62/63 = SPI0 MISO/MOSI
balls (direct conflict with the display plan); I2C4 (M2)
is on balls 4/5 in the GPIO3/MIPI-clock area. The board
DTS instantiates i2c3 at 100 kHz, status disabled [SDK].
Net: at most one I2C bus is realistically free on this
board, and which one is a pin-budget decision (open
question Q7).

QaonicOS status: unused — no driver, no syscall.
Consumers waiting: a future touch controller (the
long-term D-GUI-4 plan), the gauge-IC battery option
(power research), camera sensors (Part A).

Abstraction it implies: one new syscall, `SYS_I2C_XFER`
(bus, addr, write-then-read), kernel-polled first, DMA
never (transfers are tiny).
Leverage: **medium** — nothing needs it this quarter, but
it is the only road to touch and to a real battery gauge.

## 4. SPI0

**What it does** — The SoC's general SPI controller: two
chip-select outputs, master or slave, software-configured
modes [DS §1.2.11].

Registers/driver: SPI0 `0xFF500000` (SPI 23), compatible
`rockchip,rv1106-spi` (the rk3066-spi register family)
[SDK; draft header]. A second node, SPI1 `0xFF510000`,
exists in the inherited DTS but the datasheet counts
exactly one SPI controller for RV1103 and no SPI1 ball
exists — phantom [DS §1.2.11, §2.6].

Pico Mini B exposure: routed, M0 ball set: CLK ball 64,
MOSI ball 63, MISO ball 62, CS0 ball 65, CS1 ball 55 —
all in the GPIO1_C/D area, header-reachable, but shared
with the SDIO mux group and I2C4-M1 (see §3, §7). The
board DTS's (disabled) spidev example configures 50 MHz
[SDK]; the controller ceiling is a TRM figure not present
in local sources [?].

QaonicOS status: draft only. `kernel/kernel/arm/draft/`
holds `spi0.c` + `spi_lcd.h` + `st7789.h`: SPI0 polled
TX-only driver and the ST7789 240×240 init sequence for
the product display, compile-checked, not integrated,
with CRU clock setup and the DC/RST/BL GPIO numbers
marked TODO(HW) [code]. Hardware test H1.3 owns bring-up.

Abstraction it implies: the draft's own plan — a kernel
`SYS_SPI_XFER` with init/flush kept in the userspace
`face` program.
Leverage: **high** — this is the product's display; every
GUI phase ultimately renders through it on hardware.

## 5. SFC / FSPI — serial flash controller

**What it does** — The Flexible SPI (FSPI) controller
that talks to the boot/storage flash: 1-, 2- and 4-bit
data bus widths, DMA-capable, the BootROM's SPI boot
device [DS §1.2.2, §1.2.11-block diagram].

Registers/driver: `sfc` node at `0xFFAC0000` (SPI 53),
compatible `rockchip,sfc`, clocks SCLK_SFC/HCLK_SFC [SDK].
Register map as inventoried by the draft from the
shipping U-Boot `rockchip_sfc.c`: CTRL 0x00, ICLR 0x08,
RCVR 0x10, FSR 0x20, SR 0x24, CMD 0x100, ADDR 0x104,
DATA 0x108 [code: draft/sfc_nand.c].

Pico Mini B exposure: fully routed and populated — FSPI
balls 28–33 wire to the onboard Winbond **W25N01GV**
128 MB SPI NAND (schematic part U2; board DTS enables
`sfc` with a `spi-nand` child at 75 MHz, RX 4-bit /
TX 1-bit) [SCH, SDK]. The NAND carries the whole B0
partition layout (env/idblock/uboot/boot/oem/userdata/
rootfs per parameter.txt) [SDK].

QaonicOS status: draft only, but the most complete draft
in the tree. `draft/sfc_nand.{c,h}` implements the full
W25N01GV command path (RESET, READ_ID, GET/SET FEATURE,
WRITE ENABLE, PAGE_READ, READ_CACHE, PROG_LOAD,
PROG_EXEC, BLOCK_ERASE; geometry 2048 B page × 64 pages
× 1024 blocks; bad-block marker = first spare byte)
[code]. Two ECC facts the draft relies on: (1) error
correction is the **NAND's own on-die ECC** (Config
register 0xB0 bit4 ECC-E), not an SFC engine — the
RV1103 has no raw-NAND BCH controller in its DTS at all
[SDK, inf]; (2) littlefs gets the region blocks 320–999
so it never touches the boot partitions [code].
Meanwhile /data in QEMU is a RAM backend (FND-4 is
adding a persistent blk-backed one for emulation).

Abstraction it implies: none new at the user ABI —
littlefs already abstracts /data (syscalls 94–97); the
SFC driver becomes lfs_port's hardware backend, next to
the RAM and blk backends.
Leverage: **high** — the product's only persistent
storage; the out-of-the-box promise (provisioning,
credentials, updates surviving power cuts) physically
lives on this controller.

## 6. SDMMC

**What it does** — SD/MMC host controller: SD 3.0 and
MMC 4.51, 4-bit bus [DS §1.2.2], with an internal DMA
(IDMAC), 256-deep FIFO, and card-detect — SDMMC detect
is also a PMU wake source (power research).

Registers/driver: `sdmmc` node at `0xFFAA0000` (SPI 52),
compatible `rockchip,rv1106-dw-mshc` /
`rockchip,rk3288-dw-mshc` (Synopsys DW-MSHC register map),
SoC max-frequency 200 MHz, board-capped to 50 MHz [SDK].
(The B0 research's early guess of 0xFF7C0000 was wrong;
0xFFAA0000 is settled by the DTS [SDK].)

Pico Mini B exposure: **routed to a populated microSD
(TF) socket.** Evidence: the schematic contains the
socket (part MUP M617-2, "TF card socket, short body")
with SDMMC D0/D1 wired to its pins (PITF107/PITF108) and
the SDMMC group on balls 8–14 (DET on ball 8) [SCH];
the board DTS sets `sdmmc` status okay with the
`sdmmc0_det` pin group and 4-bit bus [SDK]; the B0
research recorded the slot as present and functional,
making the Mini B a dual-storage board (NAND primary,
TF secondary) [project docs]. One vendor quirk to not
inherit: the board DTS also says `non-removable` +
`supports-sdio`, which contradicts the socket — treat
DET honestly in our driver. BootROM boots from SD/MMC
[DS §1.2.2], which cuts both ways: SD is a
non-destructive development/recovery path (B0 §5), and
a card with a valid idbloader can shadow the NAND at
boot — a provisioning behaviour to respect, not a bug.

QaonicOS status: unused on hardware. The kernel's FAT32
(`fat32.c`) works today only over QEMU virtio-blk; the
B0 research already noted its backend seam
(`sd_read`/`sd_write`) is controller-agnostic, so a
dw-mshc driver slots underneath without touching the
filesystem [code, project docs].

Abstraction it implies: none new — a second block
device behind the existing blk/FAT32 seam (and, later,
a mount policy decision: user content vs second littlefs
volume).
Leverage: **high** — removable bulk storage, the
fastest hardware iteration loop (SD boot), and the
natural home for user media/logs that must not wear the
boot NAND.

## 7. SDIO controller

**What it does** — A second DW-MSHC instance dedicated
to SDIO: SDIO 3.0, 4-bit bus [DS §1.2.11].

Registers/driver: `sdio` node at `0xFF9A0000` (SPI 49),
same `dw-mshc` compatible and FIFO as §6 [SDK].

Pico Mini B exposure: effectively **not routable**. Its
only mux option (M1) lives on the GPIO1_C/D balls
(54–57, 60–65) — every one of those balls is already
committed on this board to SPI0, UART4, UART3 or I2C3
[DS §2.6, inf]. No SDIO device exists on the PCB [SCH].

QaonicOS status: unused, no plans.
Abstraction it implies: none — the dw-mshc driver from
§6 would cover it if a board ever routed it.
Leverage: **low** — the classic SDIO use (a Wi-Fi
module) is already owned by the ESP32-C6 over UART.

## 8. eMMC controller

**What it does** — A third DW-MSHC instance, tuned for
eMMC (`rockchip,use-v2-tuning` in its node) [SDK]; the
datasheet's external-memory section promises MMC 4.51
on a 4-bit bus [DS §1.2.2].

Registers/driver: `emmc` node at `0xFFA90000` (SPI 48),
same dw-mshc family [SDK].

Pico Mini B exposure: **not routable** — the QFN88 pin
list contains exactly one MMC pin group (the SDMMC
group of §6); there are no separate eMMC balls
[DS §2.6]. This controller is a package phantom on
RV1103 (on the QFN128 RV1106 it earns its own pins —
[inf]).

QaonicOS status: unused. Abstraction: none.
Leverage: **low** (none for this board).

## 9. USB 2.0 OTG (DWC3 + USB2 PHY)

**What it does** — One USB 2.0 port, host and device:
high-speed 480 Mbps, full/low speed [DS §1.2.11]. The
silicon controller is a Synopsys **DWC3** (so host mode
is xHCI; the datasheet's "EHCI/OHCI" wording is
Rockchip boilerplate that does not match the DWC3 DTS
compatible — flagged so nobody designs against it)
[SDK, DS].

Registers/driver: `usbdrd_dwc3` at `0xFFB00000`
(1 MB window, SPI 54), `dr_mode = "otg"`,
`maximum-speed = "high-speed"`, `phy_type =
"utmi_wide"`, plus a long list of Synopsys quirks in
the node; the PHY is `u2phy` at `0xFF3E0000` (SPI
58–60), with OTP calibration cells [SDK]. Driver model:
DWC3 TRB rings + endpoint state machines; the shipping
kernel's dwc3 + Rockchip glue is the local reference
[SDK].

Pico Mini B exposure: routed, device-first. Dedicated
balls 18 (DM) / 19 (DP) wire to the board's **USB-C
receptacle** (schematic J10; CC resistors make it a
power sink, VBUS feeds the board's VSYS rail, a
USB_DET_IN net exists) [SCH]. The board DTS enables
the controller with `dr_mode = "peripheral"` [SDK].
This port is already load-bearing in the vendor chain:
it is the MaskROM download interface (the B0 flashing
path via upgrade_tool / SocToolKit) [project docs].

QaonicOS status: **completely unused** — no host stack,
no gadget stack, no syscalls; QEMU uses virtio-net/blk
so nothing transfers. Building one stands on: the DWC3
register/TRB model, the U2PHY init sequence (GRF +
OTP cells), and a role decision. Sensible product
order: (1) **device gadget** — a CDC-ACM/DFU-style
channel for provisioning, update delivery (FND-5's
missing transport) and recovery, possibly an RNDIS/ECM
gadget as a network path that bypasses the C6;
(2) host mode (HID/storage) much later, if ever.

Abstraction it implies: a `SYS_USB_*` family does not
exist yet; the gadget should surface as a link-class
device (like the C6 link) rather than raw endpoints —
one new syscall family, sized by the gadget design.
Leverage: **high** as a device (provisioning / update /
recovery / wired data with zero radio), **medium** as
a host; with the honest caveat that a from-zero DWC3
gadget is the largest single driver in this inventory.

## 10. Ethernet — GMAC + embedded FE PHY

**What it does** — 10/100 Mbps Ethernet: a DesignWare
MAC with an **embedded 10/100 PHY on the die**, RMII
interface between them, full/half duplex, its own DMA
engine and descriptor rings, plus a dedicated wake
interrupt (`eth_wake_irq`) [DS §1.2.11, SDK].

Registers/driver: `gmac` at `0xFFA80000` (64 KB window;
SPI 93 macirq, SPI 96 eth_wake_irq), compatible
`rockchip,rv1106-gmac` / `snps,dwmac-4.20a` (the stmmac
family); `phy-mode = "rmii"`, `clock_in_out = "input"`;
the internal PHY is an MDIO device at address 2
(`ethernet-phy-id0044.1400`) with its own clock/reset
and **OTP calibration cells** (`macphy_bgs`,
`macphy_txlevel` — §18); TX/RX DMA ring sizes 256/128
in the node [SDK].

Pico Mini B exposure: **the silicon path is complete,
the board path is a footprint.** The PHY's four signal
balls are dedicated (FEPHY_TXP ball 70, TXN 69, RXP 68,
RXN 67, plus EXTR ball 72 / AVDD ball 71) [DS §2.6],
and the schematic routes the two pairs to connector
**P401**, a 5-pin header footprint carrying ETH_T_P/N
and ETH_R_P/N — the "5 castellated holes for Ethernet"
the F5 research noted [SCH]. There is **no magnetics
and no RJ45 on the board**, and the board DTS leaves
`gmac` status = **disabled** [SDK]. So wired Ethernet
is one external magnetics/RJ45 module (wired to P401)
away — a bench accessory, not a product connector.

QaonicOS status: unused. A driver would need: CRU
clock setup (incl. where the 50 MHz RMII reference
physically comes from with the internal PHY —
`clock_in_out = "input"` says the MAC consumes it;
open question Q5), GRF RMII configuration, stmmac-
class descriptor/DMA programming, MDIO + internal-PHY
init including the OTP calibration values, and then
plugging into the existing net stack as a second NIC
backend under the hand-rolled TCP (syscalls 73–77)
[code]. It would **not** use PL330 (the MAC has its
own DMA).

Abstraction it implies: none new — a NIC backend under
the existing net stack, selected at boot like the
virtio-net backend is today.
Leverage: **medium-high for development, low for the
product.** A wired bench network changes C6 bring-up
and H2 testing (deterministic, no Wi-Fi variables) and
gives the device a service port; the shipping product's
network is the C6's Wi-Fi by design, so this does not
change C6 assumptions — it demotes them from "only
path" to "product path", which is exactly what a debug
port should do.

## 11. SARADC

**What it does** — Successive-approximation ADC:
10-bit, up to 1 MS/s, **2 single-ended channels**, the
SoC's only analogue inputs [DS §1.2.12].

Registers/driver: `saradc` at `0xFF3C0000` (0x200,
SPI 62), compatible `rockchip,rv1106-saradc` [SDK].
The register map, electrical window (0–1.8 V) and the
SYS_POWER ABI design live in the power research
(`docs/riset-power-battery/`) and are not repeated;
the vendor Linux exposes both channels via IIO — the
clone's `adc/adc.c` reads `in_voltage0_raw` /
`in_voltage1_raw` [SDK]. This entry's job is the
complete routing table:

| Ch | Ball | Mux alts | Header | Occupant | Free? |
|----|------|----------|--------|----------|-------|
| IN0 | 16 | GPIO4_C0 | H1 pin 9 (PIH109) | **RECOVERY key** — schematic note: "SARADC_IN0 must always be pulled-up"; the recovery-key circuit hangs off this net [SCH] | **No** — reserved; a divider or key ladder here fights the mandatory pull-up and the recovery function |
| IN1 | 15 | GPIO4_C1 / PWM1_M1 | H1 pin 8 (PIH108) | none | **Yes** — assigned to the battery divider by the power research (200k/100k → 3:1 into the 1.8 V window) |

[DS §2.6, SCH, power research §SARADC]

QaonicOS status: unused — no driver. Note the hard
ceiling: with IN0 reserved, the product has exactly
**one** analogue input, forever; anything else
analogue must arrive via I2C (§3) or not at all.
Abstraction it implies: `SYS_POWER` (power research
PWR-1/2) for the battery; a raw channel read can ride
the same syscall family for bench use.
Leverage: **medium** — small block, but it is the
battery gauge the product ships with (until/unless an
I2C gauge IC replaces the source behind the same ABI).

## 12. DMAC — PL330

**What it does** — Microcode-programmed DMA: linked-list
scatter-gather, memory-to-memory, memory↔peripheral,
TrustZone-programmable channels [DS §1.2.3].

Registers/driver: one node — `dmac` at `0xFF420000`
(SPI 63–70 per-channel + 79), compatible `arm,pl330`,
`arm,pl330-periph-burst` [SDK]. **Discrepancy:** the
datasheet text claims "totally three embedded DMA
controllers … each 8 channels, 32 hardware requests",
but the DTS instantiates exactly one PL330 and the
block diagram draws one DMAC — silicon-as-wired is
**1× PL330, 8 channels**; the "three" is family
boilerplate until a TRM says otherwise (open question
Q1) [DS vs SDK].

Pico Mini B exposure: on-die only (no pins); every
consumer is internal.

QaonicOS status: partial — the furthest along of the
"unused" blocks. `pl330.{c,h}` stage 1 is done: a
28-byte RX microcode program for the audio I2S path,
syscalls 89–92, `UPL330: PASS` at the structure level;
actually issuing DMAGO is hardware-phase (audio plan
D2b) [code]. The dw-mshc controllers (§6–8), the GMAC
(§10) and the crypto/decompress engines have their own
DMA and do not need the PL330. Waiting consumers:
SFC bursts (§5), SPI0 display flushes (§4), UART4 at
921600 baud (§2), memory-to-memory copies.

Abstraction it implies: correctly, none at the user
ABI — DMA stays kernel-internal behind a small
`dma_request/dma_xfer` API whose embryo is `pl330.h`.
Leverage: **high as an enabler** — it is the difference
between polled toy drivers and real ones for SFC,
SDMMC-class bursts, the display and the C6 link.

## 13. Watchdog (WDT)

**What it does** — 32-bit countdown watchdog: on timeout
either a system reset directly, or an interrupt first
and a reset if the interrupt is not serviced before a
second timeout. The datasheet counts two — one secure,
one non-secure [DS §1.2.3].

Registers/driver: one node — `wdt` at `0xFF5A0000`
(SPI 46), compatible `rockchip,rv1106-wdt` (the
DesignWare APB watchdog family) [SDK].

Pico Mini B exposure: on-die only.

QaonicOS status: unused. The foundations track's FND-3
is a *software* health supervisor; this hardware WDT is
its natural final escalation rung, and the FND-1 plan
already names "PMU/WDT" as the RV1103 seam behind
SYS_RESET/SYS_POWEROFF (which QEMU serves via PSCI)
[code: psci.c, PLAN-FND]. Bootstate reason 3 (watchdog)
already exists in the boot log vocabulary.

Abstraction it implies: kernel-internal arm/kick from
the supervisor only — user programs must never pet it.
Leverage: **medium-high** — tiny driver, and it is the
only recovery rung that still works when the CPU is
wedged inside kernel code.

## 14. Timers and RTC

**What it does** — Time bases: the datasheet counts 2
secure + 6 non-secure 64-bit timers (free-running or
user-count, interrupt-driven) [DS §1.2.3]; the SoC also
has a real-time clock block with calendar/alarm.

Registers/driver: the kernel's tick comes from the ARM
generic timer (PPI, no MMIO) [hw-addrs.md]. The Rockchip
APB timers exist per the datasheet but have **no nodes
in the inherited DTS** — their base addresses are not
determinable from local sources [?]. RTC: `rtc` at
`0xFF1C0000` (SPI 98), compatible `rockchip,rv1106-rtc`
[SDK].

Pico Mini B exposure: on-die only. Per the power
research's TRM reading, the RTC sits in the VD_LOGIC
power domain, so a timed wake out of the deepest PMU
states is doubtful — HPTIMER / PMU-timeout are the
fallback wake timers there (power research §wake).

QaonicOS status: partial. Uptime/tick = generic timer
[code]; wall clock is NTP-set (SYS_TIME_SET, time
syscalls 100/101) and **dies with power** — no RTC
driver, so an offline device that loses power loses
the time of day (relevant to the out-of-box story: the
face clock is uptime-fallback today).

Abstraction it implies: small — RTC get/set folded
into the existing SYS_TIME family, plus "read RTC at
boot before NTP" as a boot-order rule.
Leverage: **medium** — offline wall clock and any
future scheduled-wake feature both stand on it.

## 15. PWM (×11 usable + PWM7)

**What it does** — Pulse-width modulators with 32-bit
counters: continuous or one-shot output, input capture
mode, and IR-optimised operation on PWM3/PWM11
[DS §1.2.3].

Registers/driver: twelve nodes — PWM0–3 at
`0xFF350000`+16 B steps, PWM4–7 at `0xFF360000`+,
PWM8–11 at `0xFF490000`+ (shared SPI 31–36),
compatible `rockchip,rv1106-pwm` [SDK]. The datasheet
says "11 on-chip PWMs (PWM0–PWM6, PWM8–PWM11)"; PWM7
nevertheless has a node and a ball mux (pin 3,
PWM7_IR_M2 shared with MIPI_CLK1_OUT) [DS §1.2.3, §2.6].

Pico Mini B exposure: routed on paper, starved in
practice — nearly every PWM mux slot collides with a
committed function [DS §2.6]: PWM1_M1 = SARADC_IN1's
ball 15 (battery), PWM10_M1 = UART4_RTSN ball 52,
PWM11_M1 = UART4_CTSN ball 53, PWM8_M0/PWM9_M0 =
SDMMC D0/D1 balls 10/9, PWM4/5/6_M2 = SPI0 balls
64/63/62, PWM0_M1 = I2C3_SDA ball 55, PWM9_M1 =
UART4_TX ball 60, PWM8_M1 = UART4_RX ball 61.
Realistically free: PWM0_M0 on ball 48 (the CPU_AVS
ball) and whichever channel the display-backlight pin
negotiation yields — the ST7789 draft currently hangs
BL on a plain GPIO (TODO(HW) in the draft) [code].

QaonicOS status: unused — no driver, no syscall (the
clone's pwm example is vendor Linux).
Abstraction it implies: one new syscall
`SYS_PWM_SET` (channel, period, duty) is plenty.
Leverage: **medium-low** — backlight dimming is a real
product want (it is also the "dim" rung of the power
policy ladder), IR transmit is a curiosity, the rest
is spoken for by pin collisions.

## 16. Crypto engine, TRNG, PKA

**What it does** — The secure-system block: a cipher
engine with linked-list DMA supporting SHA-1,
SHA-224/256, MD5 (plus HMAC), AES-128/192/256 in
ECB/CBC/OFB/CFB/CTR/CTS/XTS/**GCM**/CBC-MAC/CMAC,
DES/TDES; a PKA unit for up to 4096-bit RSA/ECC/SM2
operations; and random-number engines (one
secure-only, one security-configurable) [DS §1.2.3].

Registers/driver: `crypto` at `0xFF440000` (SPI 15),
compatible `rockchip,crypto-v3`; `rng` at `0xFF448000`
(SPI 17/20), compatible `rockchip,trngv1` [SDK].
Register semantics from the shipping crypto-v3 driver
[SDK]; no TRM locally [?].

Pico Mini B exposure: on-die only — pure leverage, no
pins, no board decision.

QaonicOS status: unused — **all** cryptography is
software today: AES-256-GCM (`aesgcm.c`) for the B5.5
credential store, SHA-256 (`updverify.c`) for PIN and
update-package hashes, software RSA-2048 verify, and
a TLS entropy source the foundation audit flags as
weak (timer + xorshift constructions; audit item #13)
[code]. The TRNG alone justifies a driver: it kills
audit #13 and gives the credstore nonces and the
ONB-2 device secret a real entropy source. AES-GCM /
SHA offload would cut the B5.5 cost on a single
1.2 GHz core; PKA could accelerate update-signature
verification (currently pure software RSA).

Abstraction it implies: kernel-internal acceleration
hooks *under* the existing software implementations
(same outputs, verified against the software path —
the pattern B5.5 already used with host vectors), plus
one honest new syscall: `SYS_RANDOM`. Keys stay in the
kernel per the B5 threat model.
Leverage: **high** — the TRNG half is small, safe, and
removes a flagged weakness; the offload half is
optional gravy on top of the same driver.

## 17. Hardware decompression (DECOM)

**What it does** — Inflate engine: GZIP / DEFLATE /
ZLIB decompression in hardware, with an output-size
limit guard against decompression bombs [DS §1.2.3].

Registers/driver: `hw_decompress` at `0xFF520000`
(SPI 112), compatible `rockchip,hw-decompress` [SDK].

Pico Mini B exposure: on-die only.
QaonicOS status: unused — update packages are
uncompressed today; the 8 KB QEMU package cap in
FND-5 is a buffer choice, not a format limit, and no
software inflate is ported either.
Abstraction it implies: kernel-internal inflate
service for the update path (and any future asset
pipeline); no user ABI.
Leverage: **low-medium** — genuinely useful for update
payload size on a 128 MB NAND, but nothing blocks on
it.

## 18. OTP

**What it does** — One-time-programmable memory:
8 Kbit total, 7 Kbit reserved to secure applications
[DS §1.2.12]. The vendor programs identity and
calibration into it at the factory.

Registers/driver: `otp` at `0xFF3D0000` (16 KB window),
compatible `rockchip,rv1106-otp`, with nvmem cells:
cpu_code @0x02, cpu_version @0x08, **unique id (16 B)
@0x0A**, cpu/log leakage bytes, and the GMAC PHY
calibration cells macphy_bgs @0x2D / macphy_txlevel
@0x2E (§10 depends on them) [SDK].

Pico Mini B exposure: on-die only.

QaonicOS status: deliberately unused. Two of its uses
are already designed-around: the B5.5 credstore device
key is a build-time placeholder whose OTP-wrapped
successor is a named hardware step, and the ONB-2
device identity currently derives from a random
secret — a documented deviation the OTP unique ID is
the honest source for [code]. Anything involving
*burning* OTP bits stays parked behind the owner's
deferred secure-boot decision (B5.4) — this inventory
recommends reads only.

Abstraction it implies: a kernel-internal `otp_read()`
for the non-secret cells (identity + calibration);
no user syscall, no write path.
Leverage: **medium** — small, but §10 and device
identity both quietly depend on it.

## 19. Mailbox (CPU ↔ MCU)

**What it does** — Hardware message passing between
the Cortex-A7 and the on-die RISC-V MCU: four mailbox
elements, each one data word + one command word + a
flag bit that can raise an interrupt, plus 32 lock
registers [DS §1.2.3].

Registers/driver: two nodes — `mailbox` at
`0xFF5C0000` (main domain) and `pmu_mailbox` at
`0xFF378000` (PMU domain) [SDK].

Pico Mini B exposure: on-die only.
QaonicOS status: unused. Whether the HPMCU is even
running under our boot chain is the threading
research's THR-4 question; this entry exists so the
interconnect picture is complete: if the MCU is ever
used — or must be politely kept out of the way — this
is the only door, and no message *protocol* for it is
documented in local materials [?].
Abstraction it implies: none yet (kernel-internal if
THR-4 ever activates it).
Leverage: **low** today; it inherits whatever leverage
THR-4 assigns the HPMCU.

## 20. SoC glue — CRU, GRF, IOC/PMUIOC, GIC

**What it does** — The substrate every other entry
silently assumes: CRU generates and gates all clocks
(4 PLLs from the 24 MHz oscillator) and issues global
and per-block soft resets; GRF holds SoC-level control
registers; IOC (and PMUIOC for the PMU domain) does
pin multiplexing, pulls and drive strengths; the
GIC-400 distributes the 121 SPIs [DS §1.2.3, SDK].

Registers/driver: CRU `0xFF3A0000` (0x20000) [SDK];
GRF `0xFF000000` (0x68000 — a single syscon where other
Rockchip chips split several) [SDK, hw-addrs.md];
IOC `0xFF538000` (0x40000); PMUIOC `0xFF388000`;
GICD `0xFF1F1000` / GICC `0xFF1F2000` [hw-addrs.md].

Pico Mini B exposure: on-die only.
QaonicOS status: partial in the worst way — the QEMU
kernel configures the virt machine's equivalents, the
draft hardware drivers all stop exactly here ("CRU
clock setup TODO(HW)" in the SPI draft [code]), and
no shared helper exists. On the real board, CRU + IOC
programming is the hidden first half of *every* driver
in this inventory (SFC, SPI0, UART4, SDMMC, GMAC, USB
all need clocks ungated and pins muxed before their
first register means anything).

Abstraction it implies: kernel-internal, written once:
`clk_enable(id)` / `clk_set_rate(id, hz)`,
`ioc_mux(pin, func)` / `ioc_pull(pin, mode)`, with the
clock IDs taken from the vendor CRU bindings [SDK].
Leverage: **high as an enabler** — invisible when
present, and the absence is currently disguised as a
TODO inside six separate drivers.

---

## 21. Memory map and interconnect

Fabric: media masters (ISP, encoder, NPU, RGA) sit on
AXI; the peripheral world lives on AHB/APB in the
`0xFF000000`–`0xFFC00000` window [DS §1.2.9 bus note,
SDK]. TrustZone partitioning is pervasive: masters and
slaves carry programmable secure state, part of system
SRAM is secure-only, and external DDR can be split
into 16 regions with per-master access control
[DS §1.2.3] — all of it unused by QaonicOS today, and
all of it owned by the (deferred) secure-boot
conversation, noted here only so the map is honest.

**DRAM.** In-package DDR2, 16-bit bus; the shipping
RV1103G1 is 512 Mbit = **64 MB** [DS §1.1, §2.1]. The
vendor boot chain treats DDR as based at `0x40000000`
(the B0 flashing work carries the load-address/DDR-map
question as an open B0.5 item); the `memory@0x0` node
in some inherited DTS files is a thunder-boot/QEMU
artifact, **not** the board [SDK, project docs].
QaonicOS under QEMU runs on the virt machine's RAM
map, so the hardware kernel link address remains a
B0.5 decision, not a fact in this document.

**System SRAM — 256 KB.** CPU-visible as
`system_sram` at `0xFF6C0000` (0x40000) [SDK]. The
vendor DTS carves it as rkisp SRAM 0x00000–0x3E000
plus an 8 KB `hpmcu_sram` window at +0x3E000 [SDK],
while the vendor TOS ini loads the HPMCU image at bus
address `0x210F6000` [threading research] — i.e. the
SRAM has a second decode window in the 0x21xxxxxx
range, and the two views' exact relationship needs
the TRM [inf, ?]. Natural QaonicOS uses once the map
is settled: fast-boot scratch, and DMA bounce buffers
immune to DDR cache-effect surprises.

**PMU SRAM — 8 KB** [DS §1.2.2]. The LPMCU's territory
(power research); no base address appears in the
local DTS [?].

**BootROM — 20 KB** [DS Fig. 1-1]. Boots the system
from SPI (FSPI) or SD/MMC, and offers code download
over USB or UART [DS §1.2.2] — the unbrickable base of
the B0 chain and of the flashing flow. Its base
address is not stated in local sources [?] and is
deliberately not guessed here.

**Address-decoding quirks worth knowing:** several
register windows are far larger than the register
files behind them (GMAC 64 KB, USB 1 MB) [SDK]; the
GRF is one 0x68000 syscon [hw-addrs.md]; and the
inherited-DTS phantoms of §22 mean an address existing
in the tree is not evidence of a reachable block.

## 22. Phantom and absent blocks

Present in the inherited DTS or in folklore, **not**
usable on RV1103 / Mini B — recorded so nobody plans
against them:

| Block | Why it is a phantom | Evidence |
|---|---|---|
| UART0, UART1 | RV1106 nodes; no balls on QFN88 | DS §2.6 |
| I2C1, I2C2 | RV1106 nodes; datasheet counts I2C0/3/4 only | DS §1.2.11 |
| SPI1 | Node exists; datasheet counts one SPI controller; no balls | DS §1.2.11, §2.6 |
| GPIO2 | Node explicitly deleted by rv1103.dtsi | SDK |
| VOP + RGB output | Node exists (vop@FF990000); RV1103 has no display-output pins — the product display is SPI for a reason | SDK, DS §2.6 |
| eMMC controller pins | Controller exists (§8); the package has no eMMC ball group | DS §2.6 |
| CAN, FlexBus | Absent from datasheet and DTS entirely | DS, SDK |
| Key-matrix / touch controller / IR receiver blocks | Absent; keys = GPIO/ADC, touch = external I2C chip, IR = a PWM mode (§15) | DS, SDK |
| 2nd/3rd PL330 | Datasheet prose claims three DMACs; one node exists | DS vs SDK (§12) |

## 23. Complete peripheral base-address table

Every `reg` node of `rv1106.dtsi` (inherited by
RV1103), extracted mechanically 2026-10-23. Cluster
key: **A** vision/media (Part A), **B** audio (audio
research), **C** this inventory, **SYS** system glue.
IRQ = GIC SPI, listed only where directly verified in
the node text; "—" means not captured, not absent.
Phantom-on-RV1103 blocks are marked † (see §22).

| Block | Base | Size | IRQ | Compatible | Cl. |
|---|---|---|---|---|---|
| GRF | 0xFF000000 | 0x68000 | — | rockchip,rv1106-grf | SYS |
| RTC | 0xFF1C0000 | 0x1000 | 98 | rockchip,rv1106-rtc | C |
| GICD | 0xFF1F1000 | 0x1000 | — | arm,gic-400 | SYS |
| GICC | 0xFF1F2000 | 0x2000 | — | arm,gic-400 | SYS |
| PMU | 0xFF300000 | 0x1000 | — | rockchip,rv1106-pmu | SYS |
| I2C0 | 0xFF310000 | 0x1000 | — | rockchip,rv1106-i2c | C |
| I2C1 † | 0xFF320000 | 0x1000 | — | rockchip,rv1106-i2c | C |
| DSM (codec digital) | 0xFF340000 | 0x1000 | — | rockchip,rv1106-codec-digital | B |
| PWM0 | 0xFF350000 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM1 | 0xFF350010 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM2 | 0xFF350020 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM3 (IR) | 0xFF350030 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM4 | 0xFF360000 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM5 | 0xFF360010 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM6 | 0xFF360020 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM7 | 0xFF360030 | 0x10 | — | rockchip,rv1106-pwm | C |
| Mailbox (PMU) | 0xFF378000 | 0x200 | — | rockchip,rv1106-mailbox | C |
| GPIO0 | 0xFF380000 | 0x100 | 5 | rockchip,gpio-bank | C |
| PMUIOC | 0xFF388000 | 0x1000 | — | rockchip,rv1106-pmuioc | SYS |
| CRU | 0xFF3A0000 | 0x20000 | — | rockchip,rv1106-cru | SYS |
| SARADC | 0xFF3C0000 | 0x200 | 62 | rockchip,rv1106-saradc | C |
| TSADC | 0xFF3C8000 | 0x1000 | — | rockchip,rv1106-tsadc | C* |
| OTP | 0xFF3D0000 | 0x4000 | — | rockchip,rv1106-otp | C |
| USB2 PHY | 0xFF3E0000 | 0x8000 | — | rockchip,rv1106-usb2phy | C |
| CSI2 DPHY HW | 0xFF3E8000 | 0x8000 | — | rockchip,rv1106-csi2-dphy-hw | A |
| DMAC (PL330) | 0xFF420000 | 0x4000 | 63–70, 79 | arm,pl330 | C |
| Crypto | 0xFF440000 | 0x2000 | 15 | rockchip,crypto-v3 | C |
| TRNG | 0xFF448000 | 0x200 | — | rockchip,trngv1 | C |
| I2C2 † | 0xFF450000 | 0x1000 | — | rockchip,rv1106-i2c | C |
| I2C3 | 0xFF460000 | 0x1000 | — | rockchip,rv1106-i2c | C |
| I2C4 | 0xFF470000 | 0x1000 | — | rockchip,rv1106-i2c | C |
| ACODEC | 0xFF480000 | 0x1000 | — | rockchip,rv1103-codec | B |
| PWM8 | 0xFF490000 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM9 | 0xFF490010 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM10 | 0xFF490020 | 0x10 | — | rockchip,rv1106-pwm | C |
| PWM11 (IR) | 0xFF490030 | 0x10 | — | rockchip,rv1106-pwm | C |
| UART0 † | 0xFF4A0000 | 0x100 | 25 | rockchip,rv1106-uart | C |
| UART1 † | 0xFF4B0000 | 0x100 | 26 | rockchip,rv1106-uart | C |
| UART2 | 0xFF4C0000 | 0x100 | 27 | rockchip,rv1106-uart | C |
| UART3 | 0xFF4D0000 | 0x100 | 28 | rockchip,rv1106-uart | C |
| UART4 | 0xFF4E0000 | 0x100 | 29 | rockchip,rv1106-uart | C |
| UART5 | 0xFF4F0000 | 0x100 | 30 | rockchip,rv1106-uart | C |
| SPI0 | 0xFF500000 | 0x1000 | 23 | rockchip,rv1106-spi | C |
| SPI1 † | 0xFF510000 | 0x1000 | 24 | rockchip,rv1106-spi | C |
| HW decompress | 0xFF520000 | 0x1000 | 112 | rockchip,hw-decompress | C |
| IOC | 0xFF538000 | 0x40000 | — | rockchip,rv1106-ioc | SYS |
| GPIO1 | 0xFF530000 | 0x100 | 7 | rockchip,gpio-bank | C |
| GPIO2 † | 0xFF540000 | 0x100 | 9 | rockchip,gpio-bank | C |
| GPIO3 | 0xFF550000 | 0x100 | 11 | rockchip,gpio-bank | C |
| GPIO4 | 0xFF560000 | 0x100 | 13 | rockchip,gpio-bank | C |
| WDT | 0xFF5A0000 | 0x100 | 46 | rockchip,rv1106-wdt | C |
| Mailbox | 0xFF5C0000 | 0x200 | — | rockchip,rv1106-mailbox | C |
| NPU | 0xFF660000 | 0x10000 | — | rockchip,rv1106-rknpu | A |
| System SRAM | 0xFF6C0000 | 0x40000 | — | mmio-sram | C |
| RGA2 | 0xFF980000 | 0x1000 | — | rockchip,rga2_core0 | A |
| VOP † | 0xFF990000 | 0x200 | — | rockchip,rv1106-vop | A |
| SDIO | 0xFF9A0000 | 0x4000 | 49 | rockchip,rv1106-dw-mshc | C |
| RKISP | 0xFFA00000 | 0x7F00 | — | rockchip,rv1106-rkisp | A |
| RKCIF | 0xFFA10000 | 0x10000 | — | rockchip,rv1106-cif | A |
| MIPI CSI2 HW0 | 0xFFA20000 | 0x10000 | — | rockchip,rv1106-csi2-hw | A |
| MIPI CSI2 HW1 | 0xFFA30000 | 0x10000 | — | rockchip,rv1106-csi2-hw | A |
| RKVENC | 0xFFA50000 | 0x6000 | — | rockchip,rkv-encoder-rv1106 | A |
| RKVENC PP | 0xFFA60000 | 0x900 | — | rockchip,rkvenc-pp-rv1106 | A |
| RKDVBM | 0xFFA70000 | 0x90 | — | rockchip,rk-dvbm | A |
| GMAC | 0xFFA80000 | 0x10000 | 93, 96 | rockchip,rv1106-gmac | C |
| eMMC † | 0xFFA90000 | 0x4000 | 48 | rockchip,rv1106-dw-mshc | C |
| SDMMC | 0xFFAA0000 | 0x4000 | 52 | rockchip,rv1106-dw-mshc | C |
| SFC | 0xFFAC0000 | 0x4000 | 53 | rockchip,sfc | C |
| RVE | 0xFFAD0000 | 0x1000 | — | rockchip,rve | A |
| I2S0 (8ch TDM) | 0xFFAE0000 | 0x1000 | — | rockchip,rv1106-i2s-tdm | B |
| USB DWC3 | 0xFFB00000 | 0x100000 | 54 | snps,dwc3 | C |

\* TSADC (temperature sensor: −40…125 °C, 1 °C
resolution, user/auto modes with alarm + reset
thresholds [DS §1.2.12]) is inventoried here for the
map; it is a two-register-class block whose only
QaonicOS relevance today is thermal safety for a
fanless product — status unused, abstraction a
`SYS_TEMP` read folded into SYS_STAT if ever wanted,
leverage low. It is counted in the summary table for
completeness.

Off-map regions (no DTS node): DRAM (SiP DDR2, vendor
base 0x40000000 — §21), PMU SRAM 8 KB (base unknown),
BootROM 20 KB (base unknown), Rockchip APB timers
(count known from the datasheet, bases unknown).

## 24. Summary table

| Name | Mini B exposure | QaonicOS status | Leverage |
|---|---|---|---|
| GPIO (§1) | Routed (header subset; bank 2 deleted) | Partial (bank-0 set/get only) | High |
| UART2–5 (§2) | 2/4/3 routed; 5 not routed | Unused (no HW driver) | High |
| I2C0/3/4 (§3) | Mux-starved; ≤1 bus realistically free | Unused | Medium |
| SPI0 (§4) | Routed (M0 balls) | Draft (display) | High |
| SFC/FSPI (§5) | Routed, NAND populated | Draft (most complete) | High |
| SDMMC (§6) | Routed — populated TF socket | Unused (FAT32 seam ready) | High |
| SDIO ctrl (§7) | Not routable (pin collisions) | Unused | Low |
| eMMC ctrl (§8) | Not routable (no balls) | Unused | Low |
| USB 2.0 OTG (§9) | Routed — USB-C, device-first | Unused (no stack) | High (device) |
| GMAC + FE PHY (§10) | Pairs at P401; no magnetics; DTS disabled | Unused | Med-high (dev) |
| SARADC (§11) | IN0 reserved (RECOVERY), IN1 free (H1.8) | Unused | Medium |
| PL330 DMAC (§12) | On-die | Partial (stage 1, audio RX) | High (enabler) |
| WDT (§13) | On-die | Unused (FND-3 is software) | Med-high |
| Timers + RTC (§14) | On-die | Partial (generic timer only; no RTC) | Medium |
| PWM (§15) | Routed but collision-starved | Unused | Med-low |
| Crypto + TRNG (§16) | On-die | Unused (all crypto is software) | High |
| DECOM (§17) | On-die | Unused | Low-med |
| OTP (§18) | On-die | Deliberately unused (reads proposed) | Medium |
| Mailbox (§19) | On-die | Unused (THR-4 owns the question) | Low |
| CRU/GRF/IOC/GIC (§20) | On-die | Partial (no shared helpers) | High (enabler) |
| TSADC (§23*) | On-die | Unused | Low |
| Memory map (§21) | SiP DDR2 64 MB; SRAM 256 KB + 8 KB | QEMU map only; HW map = B0.5 | — (foundation) |

## 25. Open questions

- **Q1.** DMAC count: the datasheet prose claims three
  DMA controllers; the DTS instantiates one PL330. Is
  there a second/third DMAC (e.g. peripheral DMAs inside
  other blocks) that matters, or is it family
  boilerplate? Needs the TRM.
- **Q2.** System SRAM decode: how do the `0xFF6C0000`
  DTS window and the `0x210F6000` HPMCU load address
  relate (two views of one SRAM?), and where does the
  8 KB PMU SRAM live?
- **Q3.** Rockchip APB timer bases: 8 timers are
  datasheet-confirmed but have no DTS nodes — where are
  they, and is one of them the right tick source for
  the deepest sleep states (vs HPTIMER per the power
  research)?
- **Q4.** DWC3 host mode: is xHCI host actually
  usable/supported by Rockchip on RV1103 (the
  datasheet's EHCI/OHCI claim contradicts the DWC3
  compatible), and does the USB-C port's wiring
  (device-first CC) permit host mode at all on this
  board?
- **Q5.** GMAC reference clock: with
  `clock_in_out = "input"` and the internal PHY, where
  does the 50 MHz RMII reference physically originate
  on this board — must be settled before any GMAC
  bring-up.
- **Q6.** SDMMC product behaviour: DET polarity with
  the real MUP socket, and the policy for the BootROM's
  SD-over-NAND boot priority (a card can shadow the
  product image — feature for recovery, hazard for
  provisioning) once our SDMMC driver exists.
- **Q7.** I2C bus arbitration: which single I2C bus
  survives the pin budget once SPI0/UART4/SDMMC/
  SARADC-IN1 are all committed (touch and gauge both
  want it)?
- **Q8.** GPIO2: is the bank absent from the die, or
  only from the package/DTS? (Determines whether
  bank-2 registers answer on hardware.)
- **Q9.** BootROM interface: base address and whether
  the MaskROM USB download protocol can be
  productised as QaonicOS's own recovery channel, or
  remains a vendor-tool-only path.
- **Q10.** SPI0 clock ceiling (TRM figure) and the
  practical ST7789 write-rate it implies for full-frame
  updates — the input the UI track's pacing question
  (THR open question 3) is waiting on.
- **Q11.** TSADC placement: the block sits at
  0xFF3C8000 next to SARADC — is its automatic-mode
  reset threshold a usable last-resort thermal guard
  for a fanless product, independent of software?
