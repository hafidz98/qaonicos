# Onboarding & Security

The device has **no keyboard** — every piece of configuration
arrives from the owner's phone. Onboarding is governed by seven
owner decisions (D-ONB-1..7, 2026-10-21); the OS-side halves
(ONB-1: PIN gate + web shell, ONB-2: QR encoder) are built and
verified, and the phone transports wait on the C6 track.

> **Key facts**
>
> - **One QR bootstraps two transports** (D-ONB-1): an Android app over BLE, or a browser over a C6 SoftAP captive portal.
> - **One device PIN, mandatory** (D-ONB-3) — the single gate for web settings, the web shell, and sensitive confirmations.
> - **The API key may be skipped** (D-ONB-2/D-ONB-6): empty key = Qabot fully offline, by design. There is no skip button and no prebuilt API.
> - **Forgotten PIN** (D-ONB-4): two tiers — physical-press PIN reset *without* data loss first; factory reset only as the last resort.
> - **Web shell defaults OFF** (D-ONB-5), with a 15-minute idle auto-off. Multi-user is deferred (D-ONB-7).

## ONB-1: PIN gate, settings, web shell (built)

Kernel side: `mach3-port/src/ext/webui.c`, reached via syscall
**103 `SYS_WEBUI`**; the HTTP router (`http.c`) keeps `/` and
`/metrics` public and session-gates `/login`, `/settings`,
`/shell` behind one admin cookie (`qaon_sess`).

- **PIN storage:** SHA-256 over a domain-separated string
  (`"qaon-pin-v1:" ‖ pin`, 4–16 chars), kept in littlefs as
  `cfg-pin` — and since B5.5, inside the encrypted credential
  store like every other secret.
- **Hardening:** constant-time compares; a throttle with
  exponential lockout (5 failures → 30 s, doubling, capped at
  900 s). Counters are RAM-only today — persisting them is a
  hardware-phase note.
- **Web shell:** a bridge between the web UI and the shell
  (states 0–3), off by default per D-ONB-5. If the executing
  shell dies mid-command, FND-2's reclaim settles the bridge so
  the web side gets a definitive answer instead of a hang
  ([Boot & Resilience](boot-resilience.md)).
- Verified by ONB1-VERIFY (19 checks after the B5.5 extension).

## ONB-2: the QR setup screen (built)

`user/face/qr.c` is a from-scratch QR encoder — byte mode, ECC
level M, versions 1–6, BCH format bits computed, penalty-scored
mask selection. The framebuffer QR was decoded with ZXing and
matches the payload **exactly** (ONB2-VERIFY PASS 4/4):

```
"QAON1:" + <device secret> + ":QaonicOS-Setup"
```

One documented deviation: no MAC-address syscall exists, so the
persisted random device secret doubles as device identity. ONB-3
separates the two when the C6 transports land. Note the secret is
xorshift-seeded today — the TRNG service (SOC-2,
[Hardware & SoC](hardware-soc.md)) is its named successor.

## Credential store (B5.5, built)

Secrets never sit in plaintext config files. `credstore.c` +
`aesgcm.c` (a self-contained AES-256-GCM, verified against
published test vectors on host and in-guest) store each secret in
its `cfg-<name>` littlefs file as:

```
"QSEC" | version | nonce[12] | ciphertext | tag[16]     (33 B overhead)
```

- Syscall **104 `SYS_CRED`** (sub 0 set / 1 get / 2 present /
  3 clear / 9 self-test). Legacy plaintext values migrate
  transparently on first touch. UCREDSTORE battery: PASS.
- **The honest caveat:** the device key is currently derived from
  a **build-time placeholder** in one seam
  (`credstore_device_key()`), documented in code as
  *obfuscation only*. An OTP-wrapped random key is the hardware
  step, and it is the difference between "encrypted at rest" and
  "encrypted at rest against someone holding the flash image".
- Placement: the store stays in the kernel by rule R3 — moving it
  to userspace would expose the key material it exists to protect
  ([boundary page](kernel-userspace-boundary.md)).

## Signed updates as a security mechanism

Update packages are RSA-2048-signed (see [Updates](updates.md));
verification and staging are one atomic kernel operation, so no
caller can stage an unverified package. The dev signing key
committed in `scripts/update-img/` is a **test key by design** —
a release key must never live on a build machine.

## Parked, by owner decision

- **Secure boot (B5.4)** — deferred 2026-10-21 ("deferred until
  fully understood"). Do not burn OTP fuses, do not re-propose it
  unprompted.
- **LLM API key / Mode A live test** — skipped for now; the device
  is designed to be fully useful offline without it.

Sources:
[`docs/rencana-onboarding/PLAN.md`](../docs/rencana-onboarding/PLAN.md),
[`PLAN-ONB1-ONB2.md`](../docs/rencana-onboarding/PLAN-ONB1-ONB2.md),
[`DEVLOG-ONB1-ONB2.md`](../docs/rencana-onboarding/DEVLOG-ONB1-ONB2.md),
[`docs/rencana-hardening/PLAN-B55.md`](../docs/rencana-hardening/PLAN-B55.md),
[`DEVLOG-B55.md`](../docs/rencana-hardening/DEVLOG-B55.md),
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§5.5, §6.2).
