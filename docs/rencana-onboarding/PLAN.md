# Onboarding & Settings Surfaces Plan

Date: 2026-10-21. Track: first-time onboarding flow + the
post-onboarding settings surfaces (web + Android app). Refines the
B3 provisioning scope split and the C6 portal sketch after the
Boot UX phase. Discussion and decision: Hafidz, 2026-10-21.

## Problem

The physical device has **no keyboard**. All configuration must
happen from the user's phone. Chicken-and-egg: the local web UI is
only reachable after the device joins Wi-Fi, but joining Wi-Fi is
itself a configuration step. Bootstrap therefore needs a
pre-network transport (BLE or the device's own SoftAP).

## Decision log

- **D-ONB-1 (Hafidz, 2026-10-21): HYBRID bootstrap.** One QR code
  on the device screen, two completion paths: the Android app over
  BLE, or a plain browser over the C6 SoftAP captive portal.
- **D-ONB-2 (Hafidz, 2026-10-21): the API key step is
  SKIPPABLE.** "If it is too gated, users who just want to try
  the device get stuck in onboarding." Skipping leaves Qabot in
  a limited mode; the key can be added later in Settings.
- **D-ONB-3 (Hafidz, 2026-10-21): one device PIN, mandatory.**
  The same PIN gates the web settings pages, the web shell,
  and sensitive-op confirmations — "an exposed shell would be
  dangerous". No separate shell password. Until a PIN exists
  (i.e. during onboarding) the settings/shell surfaces stay
  closed; the setup session is the only unauthenticated window
  and it ends at Stage 4.
- **D-ONB-4 (Hafidz, 2026-10-21): forgotten-PIN recovery has two
  tiers.** Tier 1: PIN reset without data loss — started from
  the app/portal, authorised by a **physical button press on
  the device** (proof of presence, same pattern as sensitive-op
  confirmation); settings, Wi-Fi and API key survive. Tier 2
  (last resort): full factory reset — PIN, credentials, config
  and the provisioned flag are wiped and onboarding runs again.
- **D-ONB-5 (Hafidz, 2026-10-21): the web shell is OFF by
  default.** It is enabled from Settings after PIN login (a
  USB-debugging-style toggle, per the Boot UX direction) and
  turns itself off again after an idle timeout (design detail:
  15 min). The serial shell stays the dev path.
- **D-ONB-6 (Hafidz, 2026-10-21): no prebuilt API.** If the API
  key is skipped, Qabot runs **offline** — local functions only
  — until the user adds a key. There is no anonymous/shared
  provider fallback in the product (the AI Horde anonymous proxy
  remains a dev-harness arrangement, not a shipped feature).
- D-ONB-7 (DEFERRED by Hafidz, 2026-10-21): multi-user /
  per-family-member accounts. Not yet thought through; the plan
  proceeds with the single owner PIN (D-ONB-3) and adds no
  multi-user hooks for now.

## Flow

### Stage 0 — unprovisioned boot
Boot completes (splash sequence, Boot UX). The B3 `provisioned`
flag is clear, so instead of the idle eyes the face shows a
**setup screen: a large QR code** + one line of text. While in
this state the ESP32-C6 exposes BLE provisioning and the SoftAP
`QaonicOS-Setup`. The QR payload carries: format version, device
id, a short-lived setup secret, and the SoftAP SSID — encoded so a
plain camera scan offers to join the setup AP, while the app
parses the same QR for BLE pairing.

### Stage 1 — scan
- **App path:** the Android app reads the QR, pairs over BLE
  (LESC + bonding, per the companion design in
  `docs/rencana-app-qaonicos/RENCANA.md`), and runs the wizard.
- **Browser path:** the phone joins the SoftAP; the captive
  portal serves the same wizard as web pages from the C6.

### Stage 2 — Wi-Fi
The C6 scans for networks; the SSID list is shown **on the
phone**. The user picks a network and types the password on the
phone. Credentials go to the device over BLE / the portal link.
The C6 joins the home Wi-Fi; the setup session stays alive until
Stage 4 so a wrong password can be corrected in place.

### Stage 3 — minimum viable bot config (same session)
- **Device PIN** (created here; stored as a hash — B5.5).
- **Device name** and **timezone** (default: the phone's).
- **LLM API key** (Mode A) — skippable per D-ONB-2.

### Stage 4 — done
The OS sets the B3 `provisioned` flag; the SoftAP shuts down and
BLE pairing closes. The face returns to the idle eyes and briefly
shows "Setup complete — qaon.local / <IP>".

## After onboarding: settings surfaces

- **Web settings (kernel HTTP server, extended):** today's server
  serves only the monitor dashboard (`GET /`, `GET /metrics`).
  Add PIN-gated Settings pages (name, timezone, Wi-Fi, API key,
  update, mode user/dev) and a **web shell** page — the shell is
  the keyboard-less device's console, reached over the LAN, gated
  by the PIN (D-ONB-3).
- **Android app:** the same settings over BLE/LAN plus companion
  features (notifications, sensitive-op confirmation) per the
  existing companion design.
- **Serial shell `setup`** (B3) remains as the dev/recovery path.

## What already exists

- B3: first-boot detection, `provisioned` flag (bootstate bit0),
  shell `setup` flow, NTP time gating. ✅
- Kernel HTTP server with live monitor dashboard. ✅
- Companion BLE design (GATT service, LESC, physical confirm). ✅
- C6 radio plan (Wi-Fi + BLE on the ESP32-C6; RV1103 has none). ✅

## Phases

- **ONB-1 (QEMU, no board needed):** web settings pages + PIN
  gate + web shell in `http.c`; PIN set/verify plumbing through
  bootstate/config; hostfwd-based test from the host browser.
- **ONB-2 (QEMU):** face setup screen with a real QR code
  (procedural QR encoder, version-sized to the payload) driven by
  the unprovisioned state; `setup mode` interplay with Boot UX.
- **ONB-3 (hardware/C6 firmware):** BLE provisioning service on
  the C6 + UART handoff of credentials to the OS.
- **ONB-4 (hardware/C6 firmware):** SoftAP + captive portal pages
  on the C6, same wizard steps as ONB-1's pages.
- **ONB-5 (Android):** in-app wizard over BLE, converging on the
  same post-Wi-Fi settings session as the web path.

Ordering note: ONB-1/ONB-2 are board-less and can proceed now;
ONB-3/ONB-4 wait for the C6 firmware track and hardware.

## Addendum 2026-10-21 — home model & menu entry (D-GUI)

Decided with Hafidz after the Boot UX phase:

- **D-GUI-1: Qabot (the face) is the home screen.** The menu is
  a place the user visits; idle 30 s in the menu auto-returns
  to the face (already implemented, `UI_IDLE_RETURN_S`), then
  display off, then sleep.
- **D-GUI-2: Qabot also has an icon inside the menu** (the
  agent/LLM screen), so the agent is reachable from both
  directions: it is the home, and it is also a menu entry.
- **D-GUI-3: onboarding ends with a hands-on moment.** After
  Stage 4 the face invites the user to open the menu
  themselves (press the button) and explore freely, then
  return — learned by doing once, not by reading instructions.
  A small persistent menu hint stays on the face afterwards.
- **D-GUI-4: physical buttons for now.** The menu action is an
  event mapping (`EV_MENU`), not a button that must carry an
  "M" symbol — any physical button may be assigned.
  **Touchscreen stays in the long-term plan** (touch panel
  variant + driver); when it lands, a swipe on the face is the
  same menu action as the button.
