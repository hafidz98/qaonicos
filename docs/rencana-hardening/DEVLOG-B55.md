# Dev log — B5.5 credential store

Date: 2026-10-22. Plan: `PLAN-B55.md`; parent design
`PLAN-B5.md` section 4 (threat T3: secrets read out from the
NAND). Baseline was ONB-1: PIN as SHA-256 hash (kept), API key
as PLAINTEXT in `cfg-apikey`, no online-guess protection.

## What shipped

- **`kernel/kernel/arm/aesgcm.c`** — self-contained AES-256 +
  GHASH (SP 800-38D, 96-bit nonces), encrypt direction only.
  No mbedTLS in the kernel (same call as B5.3's standalone RSA
  verifier). Proven against host-generated PyCryptodome
  vectors (3 vectors: 21-byte, empty, 54-byte plaintext) on
  the host first, then again in-guest.
- **`kernel/kernel/arm/credstore.c`** — secrets in the same
  littlefs files as before (`cfg-<name>`), format:
  `"QSEC" | ver=1 | nonce[12] | ciphertext | tag[16]`.
  Nonce = SHA-256(key ‖ boot_count ‖ write counter ‖ now_ms)
  truncated — the per-boot write counter guarantees nonce
  uniqueness within a boot, boot_count across boots.
  Legacy plaintext files are returned by get() and re-stored
  encrypted (transparent migration, verified).
- **Device key** = SHA-256 of a build-time placeholder, in one
  function (`credstore_device_key()`), documented in the code
  as OBFUSCATION ONLY. The hardware build wraps a random
  device key with the OTP-derived key (PLAN-B5 §4) — that is
  the real T3 answer; the seam is one function.
- **Syscall 104 `SYS_CRED`** (PLAN-B5 §4 "one kernel API"):
  sub 0 set / 1 get / 2 present / 3 clear / 9 self-test.
  Names `[a-z0-9-]` ≤ 16 chars, values ≤ 512 bytes. http.c
  (kernel-side) calls credstore_* directly; the settings
  handler never reads the key back (presence via sub-2 logic),
  and its form buffer for the key grew 64 → 520 bytes.
- **PIN gate hardening** (webui.c): constant-time compares
  for the PIN hash and the session token; online throttle —
  5 consecutive failures lock login for 30 s, doubling per
  lockout (cap 900 s); while locked even the correct PIN is
  refused and the login page states the remaining seconds;
  `webui_pin_verify` is tri-state (1/0/-1); setting a new PIN
  clears the throttle. Counters are RAM-only (QEMU config is
  RAM-backed anyway); persistence is a hardware follow-up.

## Bugs caught by the tests (not by inspection)

- A first draft of GHASH multiplied y·y instead of y·H (the
  shift register started from the wrong operand) — the host
  vector run caught it before the code ever reached the
  kernel; fixed and re-verified 3/3.
- The SYS_CRED get path rejected user buffers > 512 bytes
  (conflating the value cap with the buffer size); the
  one-shot's 640-byte buffer exposed it. Fixed: the buffer
  limit is a sanity bound, the value cap lives in the store.
- The V3 test vector's hex was hand-split across lines in the
  self-test and two nibbles were transposed; vectors now sit
  on single lines copied verbatim from the generator output.

## Verification

- Boot one-shot `user/ucred.c` → **UCREDSTORE: PASS**:
  self-test bitmask 0xF (3 AES-GCM vectors + kernel store
  roundtrip), userspace set/get roundtrip, raw file is QSEC
  with no plaintext substring, one flipped ciphertext byte →
  get fails, legacy plaintext migrates on get, clear →
  present 0. cfg-apikey is snapshotted/restored around the
  migration case.
- `scripts/verify-onb1.py` (now 19 checks): all ONB-1 checks
  plus B5.5 — settings save of an API key lands as a QSEC blob
  (serial `cat /data/cfg-apikey`, no plaintext), and the
  throttle flow (4 wrong → still "PIN salah"; 5th → locked
  page; correct PIN refused while locked). **ONB1-VERIFY:
  PASS**; ONB2-VERIFY: PASS.
- Regression battery (SHELL / SAFEMODE / FIRSTBOOT /
  UPDATE): PASS on a full sequential re-run. One sequential
  run showed SAFEMODE + FIRSTBOOT FAIL while both passed
  standalone immediately after and in the repeated sequence —
  the known shared-disk-state flake (killed boots between
  scripts accumulate fail_count); recorded, not hidden.

## Remaining (hardware / later)

- OTP-wrapped device key + persisted throttle counters.
- Wi-Fi PSK (`cfg-wifipsk`) slots into the same store API
  when the C6 link lands.
- B5.4 secure boot stays DEFERRED (owner decision 2026-10-21).
