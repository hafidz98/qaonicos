# B5 Design: Hardening -- secure boot, signed updates, secrets

Date: 2026-10-20. Phase B5 of the out-of-the-box roadmap.
Status: DESIGN. Most items are hardware-blocked (they need the
Pico Mini B and, for secure boot, an irreversible OTP decision).
One control is implemented now: anti-rollback in the update
manager (see "Implemented" below).

## Threat model
In scope:
- T1: an attacker flashes/runs an unsigned or tampered image.
- T2: an attacker rolls the device back to an older, vulnerable
  version (downgrade attack).
- T3: credentials (Wi-Fi PSK, API keys) read out from the NAND
  with a chip reader.
- T4: a malicious or corrupted update package is staged.
Out of scope: physical decapping/lab attacks, side channels,
a compromised signing machine (key custody is a process control).

## 1. Secure boot chain (hardware, DECISION REQUIRED)
RV1103B MaskROM can verify the idbloader against an RSA key
hash burned into OTP/eFuse. Once the hash is burned and secure
boot is enforced, ONLY loaders signed with that key will run.
- Chain: MaskROM -> idbloader (signed, Rockchip signing tool)
  -> U-Boot with verified boot (FIT image, RSA verify of the
  kernel) -> QaonicOS kernel.
- WARNING: OTP burning is one-way. A mistake bricks the board
  for anything but the signed chain. Dev boards stay UNBURNED;
  enforcement is a release-time decision (see the report's
  decision list).
- Work items (hardware phase): signing keypair generation +
  custody, loader signing in build-update-img.sh (optional
  SIGN_KEY env), FIT packaging for the kernel, U-Boot config.

## 2. Signed update packages (design; hooks into B4)
Package format (prepended to the image payload):
  magic "QUP1", version u32, payload_len u32,
  sha256(payload) 32B, signature 64B (Ed25519) over the header
  fields + sha256.
- Verification happens at STAGE time (B4 syscall sub 7 gains a
  verify step before marking staged) -- a bad package never
  reaches a trial boot.
- Crypto: Ed25519 verify via the already-vendored mbedTLS
  (or a compact standalone verifier; measure ROM cost first --
  budget: kernel raw must stay < 4M boot partition; currently
  ~3.8M, so this needs the size diet or a bigger boot slot --
  flagged as an open question in the report).
- Public key embedded in the kernel image at build time
  (scripts/gen-update-key.sh generates the dev keypair; the
  release key never lives on the build VM).

## 3. Anti-rollback (IMPLEMENTED, B4 extension)
- bootstate_update_stage rejects version <= cur_version with a
  distinct error (-2). A staged/trial flow can therefore never
  move the device backwards. Recovery to an older version, if
  ever needed, requires the physical MaskROM path (B0 guide),
  which is an intentional, hands-on operation.
- Verified by verify-update.sh (downgrade attempt rejected).

## 4. Credential storage (design)
- Secrets live on littlefs in dedicated files (wifi.psk,
  api.key) -- never in the bootstate sector, never in logs.
- At rest encryption: AES-256-GCM (mbedTLS) with a device key.
  Honest limitation: RV1103 has no secure element; on hardware
  the device key is wrapped by an OTP-derived key, which stops
  offline NAND readout (T3) but not a runtime compromise.
  In QEMU/dev the key is a build-time placeholder: obfuscation
  only, and the docs say so.
- The B3 setup flow writes credentials through one kernel API
  so plaintext never transits the shell history.

## 5. Diagnostics upload (design, P2)
- bootstate + bootlog (B2) are already persistent and bounded.
  A future `diag` shell command dumps them over the C6 link;
  upload is opt-in per the privacy stance (no silent phone-home,
  consistent with the Android Things lesson in the B-research).

## Tasks
- [x] B5.1 This design doc + threat model.
- [x] B5.2 Anti-rollback enforced in bootstate_update_stage.
- [x] B5.3 Signed update format + stage-time verification
      (DONE 2026-10-20, see below).
- [ ] B5.4 Secure boot chain (hardware; OTP decision required).
  STATUS 2026-10-21: DEFERRED by Hafidz — he does not yet feel
  he understands secure boot well enough to decide, and asked
  for caution first. No OTP burning, no chain enforcement work
  until he revisits this with a full explanation on the table.
- [ ] B5.5 Credential store implementation (with C6/B3 portal).

## B5.3 implementation notes (2026-10-20)
- Algorithm choice changed from the sketch above: RSA-2048
  PKCS#1 v1.5 + SHA-256 instead of Ed25519 -- the verifier is
  self-contained (kernel/kernel/arm/updverify.c) and measures
  4,904 bytes of text, which answers the ROM-size question:
  it fits the current boot partition headroom (~378 KB) with
  two orders of magnitude to spare. No partition change and
  no kernel diet required.
- Package = QUP1 header (magic, version, payload_len, 256-byte
  signature) + payload; signed content is header-prefix ||
  payload. Host tool: scripts/sign-update.py (openssl); dev
  test key in scripts/update-img/dev-key.pem (committed on
  purpose; release key never on a build machine).
- Kernel: syscall 98 sub 9 verifies BEFORE staging (bad sig
  -> -3; downgrade -> -2 from the B4 check); sub 10 cancels a
  staged update (also shell `update cancel`).
- Test one-shot uupdsig: UUPDSIG: PASS -- tampered payload,
  tampered version, and bad magic rejected; valid-but-old
  version rejected as downgrade; valid package stages and
  cancels. The test is state-aware (skips stage/cancel cases
  when a real update flow owns the state).
- QEMU scope cap: packages up to 8192 bytes are copied and
  verified in the kernel. Production verification streams the
  hash over the inactive NAND slot (no full-image copy).
