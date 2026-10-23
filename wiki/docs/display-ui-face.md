# Display, UI & Face

The screen is a 240×240 RGB565 panel. Two daemons render to it —
the **face** (Qabot's living home screen) and **uiapp** (menu,
settings, monitor, power) — under a kernel-held token that only
one of them holds at a time. There is no GPU and no display
controller on the RV1103: in QEMU the framebuffer is virtio-gpu;
on hardware it will be SPI0 → ST7789, the only display path the
silicon offers.

> **Key facts**
>
> - **Display token:** face is the default holder; `SYS_DISPLAY_FLUSH` from a non-holder is rejected by the kernel — there is no drawing race.
> - **Face = procedural C renderer**, drawn directly to the framebuffer (not PNG/SVG assets); concept reference RoboEyes (GPL-3.0 — concept reimplemented, code never copied).
> - **Input is physical buttons only** (EV queue); touch is a long-term plan. In QEMU, events are injected via syscall 105.
> - **LVGL v9.5 (MIT) is adopted — inside uiapp only.** The face stays hand-rolled. Qt/QML was rejected (no platform under a custom kernel; licensing/target mismatch).
> - **The gate is size:** daemon images are embedded in the kernel image — 3,907,484 B measured against the 4,194,304 B boot partition leaves **~287 KB headroom**; an LVGL uiapp is estimated at 250–400 KB. UI-1 is a measure-first go/no-go.

## Display arbitration

Syscalls 60–68 implement the token protocol: `INFO` (60),
`FLUSH` (61, arbitrary x/y/w/h rectangles), `GRANT` (62, face →
uiapp), `ACQUIRE` (63), `RELEASE` (64), `GET_EVENT` (65),
`STATUS` (66), `UPTIME` (67), `SLEEP` (68). Input events go only
to the holder, and the kernel flushes the 16-entry event queue on
every GRANT/RELEASE so no stale event crosses a handover. UI flow:
face home → MENU event → uiapp; BACK at the menu root, or 30 s of
menu idle, returns to the face. If the holder dies, FND-2's
reclaim returns the token automatically
([Boot & Resilience](boot-resilience.md)).

Console keys map to events in QEMU (W/A/S/D = arrows, Enter = OK,
Esc = BACK, M = MENU, plus escape sequences); syscall **105**
(`SYS_EVENT_INJECT`) pushes events programmatically — the path
tests use, and the same queue GPIO buttons will feed on hardware.

## The face (daemon 0)

A separate display-token process running a purely procedural
animation: squircle eyes at rest, a waveform while
listening/speaking, a sphere while thinking — states set via
syscall 78 (`SYS_FACE_POLL` family) by qabotd and friends. Idle
behaviour chains: menu idle 30 s → back to face; face idle 120 s →
display off; 300 s → face stops rendering ("sleep v1" — a display
state, not a power state; see [Power & Battery](power-battery.md)).
The face also owns first-boot setup: it renders the onboarding QR
with its own from-scratch encoder (`user/face/qr.c` — byte mode,
ECC level M, versions 1–6; ZXing-verified).

Under the final UI revision the home screen carries a small,
persistent menu hint (three thin bars + "PRESS BUTTON FOR MENU")
and EV_MENU is an *action mapping* — the physical button need not
carry an "M" symbol.

## uiapp (daemon 1)

Hand-rolled screens today: a 2×2-tile menu (rounded tiles, one
line-icon family, labels centred under icons), a status bar
(HH:MM clock; Wi-Fi/BT icons drawn dim in QEMU — honest, there is
no radio state to report; battery outline), Settings, Monitor
(real `SYS_STAT` numbers), and the Power screen (Shutdown /
Restart / Sleep Now / Back — real since FND-1). Rendering is
strip-based flushes (240×24) on a 33 ms full-repaint loop with a
5×7 font — functional, and precisely the "raw" the UI track
replaces.

## The LVGL plan (UI track)

Research verdict
([`docs/riset-ui-framework/RINGKASAN.md`](../docs/riset-ui-framework/RINGKASAN.md)):
LVGL's port surface is three callbacks QaonicOS already has —
rect-flush (`SYS_DISPLAY_FLUSH` takes arbitrary rectangles), tick
(`sys_uptime()`), and a keypad input device that maps 1:1 onto the
EV queue (button-only group navigation is a documented LVGL case).
Partial redraw also matters on hardware: one full frame is
921,600 bits over SPI (≈23 ms at an assumed 40 Mbit/s), so
invalidation-based repaint is the only sustainable model;
QEMU's virtio-gpu hides this completely.

| Phase | Content |
|---|---|
| UI-1 | Vendor LVGL + port + **measure** — the go/no-go against the ~287 KB headroom |
| UI-2 | Shell + status bar + menu proof screen |
| UI-3 | Settings subtree (incl. the Wi-Fi password-entry decision) |
| UI-4 | Monitor + Power screens |
| UI-5 | Text edit + polish/transitions |
| UI-6 | Cleanup + size audit + full verification battery |

Mitigations if the measurement fails: harder `lv_conf` trimming,
loading daemon images from littlefs instead of embedding them, or
growing the boot partition.

Sources:
[`docs/riset-ui-framework/RINGKASAN.md`](../docs/riset-ui-framework/RINGKASAN.md),
[`docs/riset-ui-qaonicos/RINGKASAN.md`](../docs/riset-ui-qaonicos/RINGKASAN.md),
[`docs/rencana-face-qaonicos/RENCANA.md`](../docs/rencana-face-qaonicos/RENCANA.md),
[`docs/SYSCALL-ABI.md`](../docs/SYSCALL-ABI.md) (display syscalls + token protocol),
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§5.1).
