# B3 Plan: Provisioning (OS side) + NTP time gating

Date: 2026-10-20. Phase B3 of the out-of-the-box roadmap.
Goal: a brand-new device boots into a guided first-boot state
(not a bare prompt), and the system knows whether its clock is
trustworthy before anything depends on it (TLS, logs).

## Scope split
- OS side (this phase, QEMU-testable): first-boot detection +
  banner + `setup` flow + persistent provisioned flag; time
  quality flag + gating API; ntp daemon marks time synced.
- ESP32-C6 side (deferred by decision 2026-10-19): SoftAP portal
  that collects Wi-Fi credentials and hands them to the OS.

## Design
### First boot
- `flags` field added to the B2 boot state (bit0 = provisioned);
  covered by the existing checksum (old state resets once).
- One-shot `ufirst` runs every boot: if not provisioned it prints
  the first-boot banner (what the device is, that Wi-Fi setup
  happens via the companion/portal, and the local `setup`
  command), and creates default config on littlefs
  (/cfg-name = "qaon", /cfg-tz = "+0700").
- Shell builtin `setup`:
  - `setup` -> status (provisioned? name, tz)
  - `setup name <x>` / `setup tz <+/-HHMM>` -> write config files
  - `setup done` -> set provisioned flag (persists across boots)
  - `setup reset` -> clear flag (factory-reset prep, cf. B4)
- Prompt hint: while unprovisioned the shell banner line at start
  reminds the user to run `setup`.

### Time gating
- New kernel flag `time_synced`, separate from `time_valid`:
  SYS_TIME_SET (manual/test set) does NOT mark synced.
- Syscall 100 SYS_TIME_QUALITY -> 0 unsynced / 1 synced.
- Syscall 101 SYS_TIME_SYNC -> mark synced (only the NTP daemon
  calls this, after validating the reply as it already does).
- Consumers: shell `time` shows "(NTP synced)" / "(not synced)";
  the TLS/Qabot layer can query quality before trusting the
  clock for certificate validation (policy hook point; dev
  default remains warn-not-block so existing tests keep passing).

## Tasks
- [x] B3.1 bootstate flags + provisioned accessors + syscall 98
      subs 3 (get flags) / 4 (set provisioned) / 5 (clear).
- [x] B3.2 Time quality: kernel flag + syscalls 100/101 + ulib
      wrappers; ntp.c marks synced after a valid reply.
- [x] B3.3 One-shot `ufirst` (banner + default config).
- [x] B3.4 One-shot `utimegate` test: quality 0 at boot, still 0
      after SYS_TIME_SET, 1 after SYS_TIME_SYNC.
      Marker: UTIMEGATE: PASS.
- [x] B3.5 Shell `setup` builtin + `time` shows sync quality.
- [x] B3.6 scripts/verify-firstboot.sh: boot1 banner -> setup
      done -> boot2 no banner -> setup reset -> boot3 banner.
      Marker: FIRSTBOOT-VERIFY: PASS.

## Out of scope
- C6 SoftAP/BLE provisioning portal (ESP track).
- TLS hard-block policy on unsynced time (hook point only).

## Verification results (2026-10-20)
- FIRSTBOOT-VERIFY: PASS (banner -> setup name/done -> boot2
  provisioned, no banner -> reset -> boot3 banner again;
  UTIMEGATE: PASS in all 3 boots).
- SHELL-VERIFY: PASS, SAFEMODE-VERIFY: PASS (no regressions).

## Known limitation (QEMU scope)
- littlefs is RAM-backed in QEMU, so config files (cfg-name,
  cfg-tz) do not persist across reboots here. The provisioned
  flag persists (blk state sector). On hardware the NAND backend
  (B1-HW) makes config persistent too.
- NTP end-to-end still untestable in the sandbox (UDP blocked);
  the daemon now marks time synced after a validated reply --
  hardware/PC test H1.1 covers the live path.
