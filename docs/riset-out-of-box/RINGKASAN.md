# Research: What a complete, plug-and-play embedded OS needs

Date: 2026-10-20. Goal: define what QaonicOS still lacks to boot and work
"out of the box, plug and play, without errors" on Luckfox Pico Mini
(RV1103: Cortex-A7 1.2 GHz, 64 MB RAM, 128 MB SPI NAND, USB 2.0).

Sources: ESP-IDF programming guide (OTA, provisioning, filesystem table),
Raspberry Pi OS firstrun mechanism, Ubuntu Core docs (recovery modes,
try-boot), Rockchip storage/boot application notes, RV1103 Datasheet V1.4
(verified locally), embedded watchdog best-practice guides.

## 0. Executive summary

QaonicOS today is a kernel + drivers + shell that boots in QEMU. A
shippable embedded product is a *system*: bootloader chain, storage
stack, provisioning, updates, recovery, and diagnostics. The gap is not
features — it is the unglamorous infrastructure that makes a device
survive first boot, power cuts, failed updates, and user error.

The single biggest gap: **there is no path from the current QEMU ELF
to a running board**. No bootloader packaging (Rockchip `update.img`),
no SPI NAND driver, no power-loss-safe filesystem. Everything else
builds on top of that.

## 1. What production embedded OSes have that hobby kernels lack

### 1.1 Bootloader chain + image packaging
Production: multi-stage boot with a defined, signed, recoverable chain.
Rockchip: BootROM (maskrom, in silicon) → `idbloader` at sector 64
(SPL + DDR init) → U-Boot → kernel. Images are packaged as
`update.img` and flashed over USB in maskrom mode (`upgrade_tool` /
`rkdeveloptool` / SocToolKit). The chain is what makes a board
unbrickable: maskrom always works, no matter how broken the flash is.
QaonicOS today: kernel ELF loaded directly by QEMU. No loader, no
packaging, no flashing story.

### 1.2 Driver model + hardware description
Production: drivers bind to devices via a device tree / probe model,
with power domains and clock/reset sequencing. Hobby kernels hardcode
addresses and init order. For RV1103 bring-up, a device tree is the
mechanism that keeps board-specific details out of driver code and
makes the same kernel image work across board revisions.

### 1.3 Storage: wear leveling + power-loss safety
NAND flash wears out (finite program/erase cycles) and dies on
torn writes. Production stacks:
- Raw NAND → **UBI + UBIFS**: UBI does wear leveling and bad-block
  management; UBIFS is the filesystem on top. Mature, but heavy.
- Small MCUs / SPI flash → **littlefs**: power-loss safe by design
  (copy-on-write), integrated wear leveling, bounded RAM independent
  of filesystem size. ESP-IDF's own comparison table: FatFS has *no*
  power-failure protection; littlefs has it integrated.
- Managed flash (eMMC/SD) → read-only squashfs root + writable data
  partition + A/B image updates.
QaonicOS today: FAT32 on a virtual SD. Fine for dev, unacceptable for
product: one power cut mid-write can corrupt the filesystem, and raw
SPI NAND without wear leveling will develop bad blocks under logging.

### 1.4 Power management
Beyond on/off: brownout detection (act before voltage collapses),
ordered shutdown on power loss (flush filesystems), sleep/wake with
state retention. QaonicOS has a power FSM (A3) — the missing part is
tying it to storage flush and brownout handling.

### 1.5 Watchdog + crash recovery
The RV1103 has a 32-bit hardware watchdog (non-secure + secure
instances; datasheet V1.4 §features) that can reset the system on
timeout. Production pattern (from field practice):
- A supervisor feeds the watchdog only when the *whole system* is
  healthy (not just one timer ISR) — detects deadlocks, not just
  a wedged CPU.
- Persist the **reset reason** (watchdog vs power-on vs brownout)
  early in boot; log it. A watchdog reset without diagnostics is
  just a mystery reboot.
- **Boot-failure counter** in non-volatile storage: increments on
  crash resets, clears after sustained healthy runtime. After N
  consecutive failures (e.g. 3), enter safe/recovery mode instead of
  reset-looping into a brick.
- Test the failure path deliberately: hang a task, verify the
  watchdog fires in the expected window.
QaonicOS today: no watchdog driver, no reset-reason capture, no
boot counter.

### 1.6 First-boot provisioning
The "plug and play" moment. Production mechanisms:
- ESP-IDF: **unified provisioning** — SoftAP or BLE, with Security 1
  (X25519 + proof-of-possession, AES-CTR) to deliver Wi-Fi credentials;
  provisioning state tracked so it runs exactly once.
- Raspberry Pi OS: `firstrun.sh` on the boot partition, executed once
  on first boot (via `systemd.run` in cmdline.txt), then self-deletes.
  Handles hostname, user, SSH, Wi-Fi, locale. The Imager pre-seeds it.
- Ubuntu Core: device identity via serial/model assertions; first
  boot performs device initialization against a store.
QaonicOS today: NVS stores Wi-Fi credentials (A4), but there is no
first-boot flow that *gets* them onto the device. With the ESP32-C6
co-MCU handling Wi-Fi, the natural split is: C6 runs a captive-portal
AP mode for provisioning; RV1103 consumes credentials over UART.

### 1.7 Firmware update with rollback
ESP-IDF model (proven at scale): two app partitions (`ota_0`,
`ota_1`) + `otadata` tracking which is active. New firmware goes to
the inactive partition, marked `PENDING_VERIFY`; it must call
"mark valid" after self-test, else the bootloader rolls back to the
previous working image on next boot. A failed update can never brick
the device. Ubuntu Core does the same via `try-boot`
(`snap_mode=try` → `trying` → revert on failure) plus full
run/recover/reinstall modes.
QaonicOS today: no update mechanism at all.

### 1.8 Logging + diagnostics
Persistent, timestamped logs across reboots; crash dumps; a way to
export them (USB serial, network upload). Without this, field
failures are undebuggable. Minimum: ring-buffer log on a dedicated
flash partition + reset-reason recorded at boot.

### 1.9 Security
- **Secure boot**: Rockchip supports it (OTP key hash + RSA-signed
  loaders). Prevents running tampered firmware. P1, not P0.
- **Credential storage**: Wi-Fi PSK / API keys must live in a
  partition with restricted access, never in plaintext logs.
- **Sandboxing**: Mach's task isolation already gives a foundation;
  keep drivers out of the kernel where feasible.

### 1.10 Time
No RTC battery on the Pico Mini (no RTC in the RV1103 feature list;
board has no battery pads) → wall-clock resets on every boot.
NTP is mandatory before any TLS/cert validation or timestamped
logging. QaonicOS already has the NTP daemon design (A4) and real
clock plumbing (F2) — the gap is *gating*: network features must
wait for (or degrade gracefully without) time sync, and first boot
with no network must still produce a usable system.

## 2. How the comparables do "plug and play"

| System | Mechanism | The trick that makes it "just work" |
|---|---|---|
| Raspberry Pi OS | `firstrun.sh` on bootfs, run once then deleted | Factory can pre-seed identity/Wi-Fi/SSH without booting the device; user edits files on any PC |
| ESP-IDF | NVS + OTA A/B + unified provisioning (SoftAP/BLE) | Phone app flow for Wi-Fi; updates can never brick (rollback); config survives updates in NVS |
| Ubuntu Core | snapd + run/recover/reinstall modes + try-boot | Recovery system always available; failed update auto-reverts; factory reset preserves network config (`ubuntu-save`) |
| Android Things (retired) | Console-managed, auto-updating | Instructive as a *negative* example: great plug-and-play, killed by Google — don't depend on someone else's cloud for core device function |

Common thread: **every one of them separates "the thing that can
always boot" (maskrom / bootloader / recovery partition) from "the
thing being updated"**, and **first boot is a designed flow, not an
accident**.

## 3. Critical gaps for RV1103 / Pico Mini specifically

1. **Boot chain (biggest gap).** Today: QEMU loads `mach3.elf`
   directly. Real board needs: `idbloader` (or minimal SPL) at sector
   64 of SPI NAND → loader that inits DDR and loads QaonicOS →
   packaged as Rockchip `update.img` → flashed via maskrom USB
   (`upgrade_tool` on Linux, SocToolKit on Windows). The wiki already
   documents the maskrom button procedure — but no image exists to
   flash. *Nothing else matters until this exists.*
2. **SPI NAND driver + filesystem.** The 128 MB SPI NAND is the real
   storage (Mini B has no SD slot). Needed: SPI NAND driver with ECC,
   then **littlefs** (recommended: small, bounded RAM, power-loss
   safe — fits the 64 MB budget; UBIFS/UBI is overkill here). FAT32
   stays only as the QEMU dev path.
3. **No RTC.** NTP on every boot; gate TLS and log timestamps on
   sync state; keep a monotonic boot counter as fallback ordering.
4. **Watchdog.** RV1103 WDT exists (verify base address in TRM —
   datasheet lists the peripheral but not the map). Wire it to a
   supervisor + boot-failure counter in a dedicated flash sector.
5. **USB device mode.** Needed as the provisioning/flashing fallback
   (USB serial for first-boot setup, maskrom already covers
   re-flashing).
6. **Wi-Fi via co-MCU.** Provisioning flow belongs on the ESP32-C6
   (AP + captive portal); RV1103 side needs the UART protocol
   (already drafted in F5) plus "wait for network" boot logic.
7. **64 MB RAM budget.** Every subsystem above must be sized for it:
   littlefs over UBIFS, no journaling FS, bounded log buffers,
   XIP or streaming for large assets.

## 4. Prioritized: MUST-HAVE vs NICE-TO-HAVE

Acceptance bar for "boots and works out of the box without errors":
- 100/100 cold boots reach a working shell/GUI, no manual steps.
- 100 random power cuts → filesystem intact, boots cleanly after.
- Failed update → previous version boots automatically.
- 3 consecutive crashes → recovery/safe mode, never a reset loop.
- First boot with no network → device tells the user what to do
  (AP mode / on-screen prompt), doesn't hang.

### P0 — MUST-HAVE (unshippable / unreliable without)
1. **Hardware boot chain**: `idbloader` + loader + `update.img`
   packaging + documented maskrom flashing. (No board boot = no product.)
2. **SPI NAND driver + littlefs** (or equivalent power-loss-safe FS
   with wear leveling). FAT32-on-SD is dev-only.
3. **Hardware watchdog + boot-failure counter + safe-mode fallback.**
   This is the "without errors" guarantee.
4. **Reset-reason capture + persistent logging.** Can't fix what you
   can't see.
5. **First-boot provisioning flow** (Wi-Fi via C6 AP portal; device
   identity/hostname). This is the literal "plug and play."
6. **Time-sync gating**: NTP before TLS; sane behavior without
   network.

### P1 — SHOULD-HAVE (robust product)
7. **A/B (or recovery-partition) updates with automatic rollback.**
8. **Factory reset** that preserves network config.
9. **USB-serial provisioning fallback** (no Wi-Fi / headless debug).
10. **OTA download path** (HTTPS via C6, signed images, resume).

### P2 — NICE-TO-HAVE (harden later)
11. Secure boot (Rockchip OTP + signed loaders).
12. Encrypted credential partition.
13. Deep-sleep power optimization, brownout-driven flush.
14. Remote diagnostics upload.

### Suggested sequencing (follow-up plan phases)
- **B0** boot chain: idbloader + update.img + flash docs (unblocks HW).
- **B1** storage: SPI NAND driver + littlefs + power-cut testing.
- **B2** resilience: WDT driver + boot counter + reset-reason + logging.
- **B3** provisioning: C6 AP portal + first-boot flow + NTP gating.
- **B4** updates: A/B layout + rollback + factory reset.
- **B5** hardening: secure boot, OTA, USB fallback.

Each phase: research doc + implementation + hardware test plan entry
(H-series), same as the F1–F5/D1–D4 pattern that worked.
