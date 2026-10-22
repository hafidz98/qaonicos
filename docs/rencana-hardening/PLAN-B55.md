# B5.5 Implementation Plan: Credential store

Date: 2026-10-22. Parent design: `PLAN-B5.md` section 4.
Baseline: ONB-1 stores the PIN as a SHA-256 hash (fine) but the
API key as PLAINTEXT in littlefs (`cfg-apikey`), the PIN verify
has no online-guess protection, and secret writes go through the
generic cfg path. This phase implements the section-4 design in
QEMU scope.

## Scope

1. **Encrypted secret files (kernel credential store).**
   - New `kernel/kernel/arm/aesgcm.c`: self-contained AES-256 +
     GHASH (GCM), encrypt/decrypt AEAD. No mbedTLS in the kernel
     (same call as B5.3's standalone RSA verifier: size + no
     allocator/OS deps). Correctness proven against host-generated
     test vectors in a boot self-test (below), not by eyeballing.
   - New `kernel/kernel/arm/credstore.c`: 
     - File format in the SAME littlefs file as today
       (`cfg-apikey`, future `cfg-wifipsk`): magic "QSEC",
       version byte, 12-byte nonce, ciphertext ‖ 16-byte tag.
     - `credstore_set/get/present/clear(name, ...)`.
     - Nonce: SHA-256(device key ‖ boot_count ‖ write counter ‖
       now_ms), first 12 bytes. The RAM write counter increments
       on every encrypt, so nonces never repeat within a boot;
       boot_count separates boots.
     - Legacy migration: a file without the QSEC magic is read
       as legacy plaintext and rewritten encrypted on the next
       set (and by `credstore_get` callers that pass a migrate
       flag — the settings read path does this once at boot of
       the web flow... simpler: get() migrates transparently:
       read plaintext, re-store encrypted, return plaintext).
   - Device key: `credstore_device_key()` is the single seam.
     QEMU/dev build: SHA-256 of a build-time placeholder string.
     The docs and the header say plainly: this is obfuscation
     against casual readout only; the hardware build wraps a
     random device key with the OTP-derived key (PLAN-B5 §4),
     which is what actually answers threat T3.
2. **One kernel API for secrets (PLAN-B5 §4 requirement).**
   - Syscall 104 `SYS_CRED`: sub 0 set, sub 1 get, sub 2 present,
     sub 3 clear, sub 9 self-test (AES-GCM vectors, returns a
     pass bitmask — same pattern as SYS_LFS_RUNTEST).
   - Names: `[a-z0-9-]`, ≤ 16 chars, mapped to `cfg-<name>`;
     values ≤ 512 bytes. Kernel-internal callers (http.c) use
     credstore_* directly.
   - http.c settings: API key save/clear/present go through the
     store; the settings page still never renders the key.
3. **PIN gate hardening (same surface).**
   - Constant-time compares for the PIN hash and the session
     token (the current compare exits at the first difference).
   - Online throttle in webui.c: 5 consecutive failures lock
     login for 30 s; each further lockout doubles (cap 900 s);
     a success resets the counters. While locked, even the
     correct PIN is refused and the login page says how long to
     wait. `webui_pin_verify` becomes tri-state (1 ok, 0 wrong,
     -1 locked). State is RAM-only: in QEMU every config file is
     RAM-backed anyway; persisting the counters is a hardware
     follow-up (noted, not hidden).

## Verification

- New one-shot `user/ucred.c` → **UCREDSTORE: PASS**:
  self-test vectors via SYS_CRED sub 9; set/get roundtrip;
  raw file (via lfs syscalls) starts with QSEC and contains no
  plaintext; one flipped ciphertext byte → get fails; legacy
  plaintext file migrates on get; clear → present = 0.
  The one-shot snapshots and restores any pre-existing
  cfg-apikey bytes (state-tolerance rule from B5.3).
- `scripts/verify-onb1.py` gains: after saving an API key via
  settings, serial `cat /data/cfg-apikey` shows QSEC, not the
  key; throttle flow (5 wrong PINs → locked message; correct
  PIN refused while locked; reboot → login works).
- Full regression battery: SHELL / SAFEMODE / FIRSTBOOT /
  UPDATE / ONB1 / ONB2.

## Out of scope

- OTP-wrapped device key, secure boot (B5.4) — hardware +
  deferred decision.
- Wi-Fi PSK storage — no Wi-Fi stack until the C6 lands; the
  store API is name-generic so it slots in unchanged.
- Persisted throttle counters (hardware follow-up).
