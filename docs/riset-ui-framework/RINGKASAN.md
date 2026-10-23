# Research: UI framework options for QaonicOS — LVGL vs Qt/QML

Date: 2026-10-23. Scope: RESEARCH ONLY. The owner judges the
current hand-rolled UI "still very raw" and asked for a check of
LVGL and Qt/QML. This document verifies the local stack first,
then judges each candidate against it. No code was changed.

Method: local code read (`user/uiapp/*`, `user/face/*`,
`user/ulib/ulib.h`, `kernel/build-md.sh`, sizes measured with
`llvm-size` on the 2026-10-23 build) + vendor primary sources
(lvgl.io, doc.qt.io, segger.com) — URLs in §10. Vendor numbers
are marked **[vendor]**; locally measured numbers are marked
**[measured]**; engineering estimates are marked **[est]**.

## 1. Local baseline (verified, not assumed)

| Fact | Value | Source |
|---|---|---|
| Display | 240x240 RGB565, no GPU; virtio-gpu in QEMU, SPI ST7789 on HW | `ui_draw.h`, draft `spi0.c` |
| Flush ABI | `SYS_DISPLAY_FLUSH(61)`: **x, y, w, h, buf, len** — arbitrary rectangles, not strip-only | `ulib.h:37` |
| Flush usage today | uiapp and face both flush 240x24 strips (11,520 B) | `ui.c render_strips`, `face/main.c:104` |
| CPU / RAM | RV1103: 1x Cortex-A7 @ 1.2 GHz; **64 MB total** for kernel + all programs | project baseline |
| OS surface | Mach 3 + extension syscalls; **no POSIX, no libc** beyond project `ulib`; programs are fully static ELFs loaded by the kernel | `ulib.h`, `build-md.sh` |
| Input | buttons only; kernel event queue `EV_UP/DOWN/LEFT/RIGHT/OK/BACK/MENU/TICK` via `SYS_DISPLAY_GET_EVENT(65)` | `ulib.h:41-55` |
| uiapp today | text 34,500 B, bss 26,861 B, image 61,505 B **[measured]**; embed cap in build script 65,536 B (a build parameter — `utlscli` uses 1 MB) | `llvm-size`, `build-md.sh:173` |
| face today | text 18,002 B, image 34,261 B **[measured]**; procedural eyes/waveform/sphere, fixed-point trig, own QR encoder (`qr.c`, 451 lines) | `llvm-size`, `face_draw.c` |
| Kernel image | text+data **3,903,036 B [measured]**; daemon images are *embedded into the kernel image* (`extern *_img` in `user.c`) | `llvm-size mach3.elf` |
| Boot partition | `0x2000` sectors = **4,194,304 B** → headroom for kernel growth ≈ **291 KB [measured]** | `scripts/update-img/parameter.txt` |
| Screens | menu (2x2 tiles), settings list, mon, power, wifi, ble, llm, passkey, textedit (custom char picker); screen stack depth 4, `screen_t{enter,exit,on_event,render}` | `scr_*.c`, `screen.h` |
| Render loop | every 33 ms the *entire* screen is re-rendered and flushed, idle or not | `uiapp/main.c` `FRAME_MS` |
| Font | `font5x7.h`: 5x7 px, uppercase + digits + few punctuation, **no anti-aliasing**, 6 px/char fixed pitch | `ui_draw.c`, `font5x7.h` |
| Vendoring model | littlefs is vendored as a globbed source dir with its own flags in `build-md.sh` (`kernel/arm/littlefs/*.c`) | `build-md.sh:458` |

The owner's "raw" is accurate and specific: aliased 5x7 text,
every glyph/gauge hand-plotted per screen (the Wi-Fi icon is a
hand-tabled pixel-offset array in `ui.c`), no widget/focus
system beyond per-screen `sel` integers, no animation or
transition machinery, and a full repaint every frame.

## 2. The constraints that decide this

1. **Flash, not RAM, is the wall.** 64 MB of RAM makes every
   candidate's RAM footprint survivable. But the kernel image
   (with all daemon images embedded) has only **~291 KB of
   headroom** in the fixed 4 MiB boot partition. Any framework
   whose code lands in `uiapp` is paid out of that 291 KB
   unless we also change the image layout (see §7 costs).
2. **Static ELF, freestanding C, no libc/POSIX.** The candidate
   must compile with our clang freestanding flags and link
   against a shim we control, or it does not run at all.
3. **Buttons only.** The framework's keyboard/keypad focus
   model *is* the UX; pointer-oriented toolkits need not apply.
4. **SPI bandwidth on hardware.** One full 240x240 RGB565
   frame = 921,600 bit. At an assumed 40 Mbit/s SPI clock
   that is 23 ms of pure transfer; at 24 Mbit/s, 38 ms.
   Sustained full-frame 30 fps ≈ 27.6 Mbit/s before command
   overhead. (The real clock is a `TODO(HW)` in `draft/spi0.c`;
   treat these as planning numbers.) Partial redraw is not an
   optimisation on hardware, it is the only sustainable model.
   QEMU's virtio-gpu hides this completely — captures validate
   pixels and logic, never pacing.
5. **Apache 2.0 project.** Copyleft that reaches the combined
   binary, or per-unit royalties, are strategic decisions, not
   implementation details.

## 3. LVGL

### 3.1 Version, license, maturity

- Current stable: **v9.5** (released Feb 2026; 9.4 in Nov
  2025) — the 9.x line is actively developed, with an
  LVGL Pro/XML editor effort on top. **[vendor: lvgl.io blog,
  forum]**
- License: **MIT** — fully compatible with Apache 2.0
  distribution, commercial use, static linking. No royalties,
  no attribution beyond the license notice.
- Pure C (C99+), **zero external dependencies**; "just copy it
  into your project and compile it" is the documented
  integration model — the same shape as our littlefs vendoring.
- Community: the default embedded-GUI choice of the last
  years; ESP-IDF/Zephyr/NuttX/RT-Thread ship it as a component;
  a large forum + 100+ examples. Maturity risk is low; API
  churn inside 9.x is real but manageable by pinning a version.

### 3.2 Port surface — maps onto what we already have

LVGL needs exactly three things from the platform, and all
three have a native counterpart here:

| LVGL port item | Our counterpart |
|---|---|
| `flush_cb(disp, area, px_map)` + `lv_display_flush_ready()` | `sys_display_flush(area->x1, area->y1, w, h, px_map, bytes)` — the ABI already takes arbitrary rectangles (today's strips are just our choice) |
| Tick source (`lv_tick_set_cb` / `lv_tick_inc`) | `sys_uptime()` (ms) — one line |
| Input device | `LV_INDEV_TYPE_KEYPAD` with a `read_cb` draining `sys_display_get_event()`; mapping is 1:1: `EV_UP/DOWN/LEFT/RIGHT -> LV_KEY_*`, `EV_OK -> LV_KEY_ENTER`, `EV_BACK -> LV_KEY_ESC` |
| Draw buffers | `LV_DISPLAY_RENDER_MODE_PARTIAL` with 1–2 buffers of 240x24 = 11.25 KiB — *exactly today's strip size*; LVGL then flushes only invalidated areas instead of our current full repaint |
| Allocator/libc | `lv_conf.h` can select LVGL's **built-in** TLSF allocator and built-in string/printf implementations (`LV_STDLIB_BUILTIN`) — designed for no-libc targets; we still owe a small shim (`memcpy`/`memset` family, see UI-1) |
| Main loop | `lv_timer_handler()` in place of the 33 ms unconditional render; the display-token acquire/release and 30 s idle-return logic stay ours, wrapped around it |

Button-only navigation is a first-class LVGL citizen, not a
hack: widgets join a **group**, the keypad/encoder indev moves
focus, focused widgets get `LV_STATE_FOCUSED` styles, and the
docs even describe driving the *encoder* model from 3 buttons
(`LV_KEY_ENTER/LEFT/RIGHT`). Our 7 physical keys are luxurious
by LVGL standards.

### 3.3 Footprint, computed for our configuration

Vendor figures, labeled honestly:

- Library minimums **[vendor: lvgl.io/features]**: ~100 KB
  flash for the library (feature-dependent), RAM 4 KB +
  ~150 B/widget, "~48 KB for a UI with a few screens", draw
  buffer > 1/10 screen.
- Realistic simple UI **[vendor: LVGL v9.5 README]**: ~100 KB
  RAM, **~200–300 KB flash**, 1/10-screen buffer.

Our config (240x240 RGB565, partial mode, one 14–16 px font
family + one display font, widgets: button/label/list/chart/
arc/bar/switch/slider/textarea; themes on; FreeType, file
system drivers, observers, XML, 3D all OFF):

- Draw buffer: 11.25 KiB (same as today) or 22.5 KiB double.
- Runtime RAM delta vs today: **[est] +60–180 KB** (LVGL pool
  64 KiB + objects + buffer) — **irrelevant against 64 MB**.
- uiapp image: from 61.5 KB today to **[est] 250–400 KB**.
  Against the ~291 KB kernel headroom this is the whole
  problem: a naive LVGL build **does not fit the current boot
  partition**. A hard-trimmed `lv_conf` (only the widgets we
  use, one font size compiled in + subsetted generated fonts)
  can go well below the marketing range — how far is unknown
  until measured (§9, risk 1). The structural fixes are known
  and independent of LVGL: raise the uiapp embed cap (trivial),
  load daemon images from littlefs instead of embedding them
  (kernel work, also helps every future daemon), or grow the
  boot partition in the flash layout (rootfs has room; but it
  changes the hardware flashing/update story).

### 3.4 Fonts — the single biggest visible upgrade

- Built-in: Montserrat 12–48 px at **bpp=4 (anti-aliased)**,
  plus FontAwesome symbols — `LV_SYMBOL_WIFI`,
  `LV_SYMBOL_BLUETOOTH`, `LV_SYMBOL_BATTERY_*` replace our
  hand-plotted status icons directly.
- `lv_font_conv` generates **subsetted** fonts from any TTF at
  build time (only the glyphs we print, at the sizes we use) —
  the right model for flash discipline and for matching the
  approved mock's typeface.
- Runtime options if ever needed: binary fonts loaded from
  littlefs (binfont loader), and **tiny_ttf** (stb_truetype
  based) for true TTF rendering — costs CPU per glyph and a
  glyph cache; a later luxury, not the plan.
- Text gains real layout: wrapping, alignment, letter
  spacing, scrolling labels, UTF-8. Our current renderer folds
  lowercase and can't wrap at all.

### 3.5 Widgets mapped to our screens

| Our screen | LVGL construction | Notes |
|---|---|---|
| Status bar | objects on `lv_layer_top()`, 1 s timer updates clock label; symbol-font icons | cheaper than today: no per-frame repaint |
| Menu 2x2 | grid/flex container of 4 `lv_button` tiles (label + icon image), group focus | our hand-drawn line icons become small converted images or `lv_line`/`lv_arc` objects |
| Settings | `lv_list` with 4 buttons | direct replacement of the `sel` list pattern |
| Monitor | `lv_chart` / `lv_bar` / `lv_arc` + labels on a 500 ms timer from `SYS_STAT` | replaces hand bars |
| Power | `lv_list` (Sleep / Restart / Shutdown) | same actions, real widgets |
| Wi-Fi | `lv_list` of SSIDs; password via `lv_textarea` + `lv_keyboard` | **the awkward one** — a full keyboard driven by 4-way keys works via group focus but is slow; keeping our char-picker logic as a custom dialog may serve better. Decide in UI-3/UI-5, not now |
| BLE / LLM / Passkey | lists + labels + switch/checkbox | straightforward |
| Text edit | `lv_textarea` (+ keyboard) or existing picker re-hosted | see Wi-Fi note |
| QR (setup) | lives in **face**, not uiapp — untouched. If a uiapp QR is ever needed, `lv_qrcode` is built in | — |
| Transitions | screen-load animations + style transitions are built in | the "polish" the owner is missing |

### 3.6 Tooling — the honest version

- **PC simulator exists** (SDL-based port; the same UI code
  runs on the host). For us it is a fast-iteration bonus; the
  ground truth stays the QEMU capture methodology.
- **SquareLine Studio is no longer LVGL's partner tool**:
  LVGL ended the collaboration (Aug 2025). SquareLine's free
  Personal tier is non-commercial and capped (max 10 screens /
  150 widgets, per third-party reporting of their pricing
  page); commercial tiers are subscription. **[third-party]**
- LVGL's own successor is the **LVGL Pro Editor** (XML UI
  definitions, v1.0 RC in 2026) — optional, commercial-leaning
  tooling. The C API needs none of it. Plan assumes
  hand-written C, as today.

### 3.7 The face stays custom — argued

LVGL *could* host the face: an `lv_canvas` (240x240 canvas =
115,200 B RAM) or a custom widget whose draw event runs our
existing code. But: (1) the face owns display token 0 as a
separate process with its own power/FSM discipline — setup QR,
finale, sleep — that must not depend on a widget tree's timer
loop; (2) the face is a **procedural animation surface**
(fixed-point eyes/waveform/sphere), not widget UI — LVGL would
not delete one line of its drawing code, only wrap it;
(3) linking LVGL into face too pays the flash cost twice.
The face's look is a deliberate spec (procedural, living
document), not framework debt. **Verdict: face stays
hand-rolled; LVGL is adopted inside uiapp only.** Revisit only
if the face grows text-heavy states.

### 3.8 Migration effort, per piece [est, focused dev days]

| Piece | Effort | Risk driver |
|---|---|---|
| Vendor + build block in `build-md.sh` + `lv_conf.h` trim | 0.5–1 d | config surface (~300 switches) |
| Port layer (flush/tick/keypad indev + libc shim + smoke screen) | 1–2 d | freestanding friction (§9 risk 2) |
| Main loop + status bar on top layer | 0.5–1 d | token acquire/release interplay |
| Menu screen (proof screen) | 0.5–1 d | icon assets conversion |
| Settings + BLE/LLM/Passkey lists | 1–1.5 d | — |
| Wi-Fi screen + password entry | 1–2 d | keyboard-via-buttons UX |
| Monitor + Power | 0.5–1 d | — |
| Textedit decision + polish/transitions + theme to mock | 1–2 d | taste iterations |
| Cleanup (delete replaced ui_draw screens), size audit, full verify | 0.5–1 d | boot-partition fix may land here |

Total **[est] ~7–12 focused days**, phased in §8 so every
phase ends with a bootable, capturable uiapp.

## 4. Qt / QML

"Qt" is two different products here; they fail for different
reasons.

### 4.1 Qt proper (Widgets / Quick, open-source edition)

- **Platform reality:** Qt 6 runs on operating systems —
  Linux (eglfs/linuxfb), Windows, macOS, Android, QNX,
  INTEGRITY, VxWorks, WASM. Its platform abstraction (QPA)
  assumes a POSIX-ish OS: threads, filesystem, event loop,
  dynamic loading. There is **no bare-metal or custom-kernel
  port**, and writing one means porting QtCore itself to
  Mach 3 — a project larger than QaonicOS. On RV1103 the
  realistic Qt route would be "run Linux", i.e. abandon the
  OS this project exists to build.
- **Footprint:** a minimal Qt Quick stack is tens of MB of
  shared libraries and wants OpenGL ES or a software GL path;
  against a 4 MiB boot partition and 64 MB total RAM it is
  not within an order of magnitude of fitting.
- **Licensing collision:** core modules are LGPLv3 (several
  add-ons GPLv3-only). LGPLv3 requires that a user be able to
  modify the library and **relink** it into the application,
  plus Installation Information duties for a consumer device.
  Our loader accepts only fully static, monolithic ELFs and
  the roadmap is toward signed updates — the exact opposite
  of user-relinkable. Dynamic linking would need a dynamic
  loader that does not exist. GPLv3-only modules would
  additionally force the combined binary out of Apache 2.0.
- **Verdict: not viable** — technically (no platform) and
  legally (static-link/relink conflict), either alone suffices.

### 4.2 Qt for MCUs (Qt Quick Ultralite)

- **What it is:** QML compiled to C++, a small runtime,
  bare-metal/FreeRTOS targets, its own platform-porting guide.
  Technically the only Qt that could ever run here.
- **Requirements [vendor: doc.qt.io]**: Cortex-M4/M33 or
  higher (RV1103 is Cortex-A7 — outside the stated class);
  flash ≥ 500 KB; static RAM ~20 KB minimal / 200 KB+ typical
  plus framebuffer. Qt's *own minimal example* measures
  `.text` = 347,552 B + 21,736 B font resources — already
  larger than our entire remaining boot-partition headroom,
  before our application exists.
- **Platform reality:** supported targets are NXP / Renesas /
  STM / Infineon / Espressif boards on bare metal or
  FreeRTOS. Rockchip is not supported; a port to our Mach 3
  extension layer would be **our** work, using a porting kit
  that ships only inside the commercial SDK, with a CMake +
  ARM-GCC/IAR + C++ runtime toolchain model (their linker
  scripts carry `.init_array`, static constructors, C++
  allocation) that our freestanding C build and flat-image
  loader do not provide.
- **Licensing [vendor: doc.qt.io]: "Qt for MCUs is available
  under commercial license from The Qt Company."** There is no
  open-source edition of Ultralite. The Qt device model is
  per-developer licenses **plus per-device distribution
  licenses**; qt.io publishes no list price (quote-based).
  Third-party reporting of Qt for Device Creation puts
  developer licenses in the ~€5,800/dev/year range and
  distribution packs starting around €15,000 — **[third-party,
  2023 figures]** — either way: recurring cost and a
  proprietary core inside an Apache-2.0 open project, for a
  hobby-scale device program.
- **Verdict: rejected.** Not on QML's merits (the tooling is
  genuinely good) but on the compound of: commercial-only
  license + royalty-bearing distribution, unsupported SoC,
  a platform port we would have to write and maintain, a
  C++/CMake toolchain graft, and a runtime that alone busts
  the boot partition.

## 5. Other credible options (brief)

- **Nuklear** (public domain, single-header C89, ~18k LOC):
  immediate-mode toolkit that emits draw commands; we would
  write the rasterising backend onto our strips ourselves.
  Technically portable here (no deps at all), tiny, and its
  font baker can produce AA-ish atlases. But it brings no
  retained widget tree, no focus/group navigation model
  comparable to LVGL's, no animation/layout system to speak
  of — we would still hand-build most product behaviour, now
  inside someone else's idiom. Gains over evolving `ui_draw`
  are real but modest.
- **U8g2** (BSD-style license): an excellent **monochrome**
  library (page/full buffers for 1-bit OLED/LCD controllers,
  700+ bitmap fonts). Our display is 240x240 **RGB565 colour**
  and the product UI needs colour tiles, gauges and AA text;
  u8g2's colour model does not exist and it has no widget
  system. Wrong tool, respectfully.
- **SEGGER emWin** (commercial): technically the closest
  LVGL peer — bare-metal C, widgets, AA, AppWizard tooling,
  PC simulation; licensing is a one-time royalty-free fee
  **[vendor: segger.com]**. But for Rockchip silicon there is
  no bundled free license (those exist via NXP/Renesas/
  Infineon/STM partnerships), source licensing is a paid
  product, and a closed core GUI contradicts the project's
  Apache-2.0 openness. Rejected on license/cost/strategy,
  not on technology.
- **Evolve `ui_draw` into our own widget layer ("ui2")**:
  zero license/size risk and keeps the strip model; the work
  list is known — offline-baked 4 bpp fonts (a host-side baker
  + a new text renderer), a focus/widget mini-framework
  (list, tiles, slider, gauge), clipping, and a transition
  helper. Every hour of polish is ours forever, and we would
  be re-deriving, badly and slowly, several percent of LVGL.
  This is the baseline LVGL had to beat — and the fallback if
  the LVGL spike (§9) fails on size.

## 6. Comparison table

| Criterion | LVGL 9.5 | Qt 6 (OSS) | Qt for MCUs (Ultralite) | Nuklear | U8g2 | emWin | Evolve ui_draw |
|---|---|---|---|---|---|---|---|
| Flash (lib, realistic UI) | 200–300 KB [vendor]; trimmable, TBD | tens of MB | ≥500 KB, min. example .text 348 KB [vendor] | ~50–100 KB [est] | small | ~100–200 KB [est] | ~0 (already ours) |
| RAM | ~48–100 KB + buffers [vendor] | 50 MB+ class | 20–200 KB+ + fb [vendor] | tens of KB | KBs (1-bit buffers) | ~tens of KB | as today |
| License | **MIT** | LGPLv3/GPLv3 | **commercial only** + distribution licenses | public domain | BSD-style | commercial, royalty-free fee | Apache 2.0 (ours) |
| Port to our kernel | port layer only: flush/tick/indev + shim (days) | port Qt itself (infeasible) | write a platform port (weeks–months) + C++/CMake graft | write draw backend (days) | n/a (mono) | vendor port exists pattern (days–weeks) | none |
| Font quality | AA bitmap (bpp4), generated TTF subsets, tiny_ttf option | excellent (vector) | very good (pre-rendered/vector hybrid) | baked atlas, fair | bitmap, 1-bit | AA fonts, converter tool | must build our own AA path |
| Widgets | 30+: list/chart/arc/keyboard/qrcode… | full | QML controls subset | basic set | none | full set | build each |
| Animation/transitions | built-in engine | excellent | QML timeline | do-it-yourself | none | built-in | do-it-yourself |
| Button-only nav | groups + keypad/encoder indev, documented | focus works, pointer-first | QML focus, pointer-first | weak | n/a | keypad support | already ours |
| Tooling | PC simulator; Pro Editor optional; SquareLine split (2025) | Qt Creator/Design Studio | Qt Creator/Design Studio (paid SDK) | none | none | AppWizard | none |
| Risk | size vs boot partition; API churn (pin version) | non-starter | license cost + unsupported SoC + port burden | partial payoff | wrong display class | closed core, cost | permanent DIY tax |

## 7. Recommendation

**Adopt LVGL 9.5 inside `uiapp` only; keep the face custom;
do not pursue either Qt variant.** LVGL is the only candidate
that simultaneously: is MIT (zero license friction with
Apache 2.0), is dependency-free C that fits our vendoring and
freestanding build, has a port surface that is three callbacks
we already own equivalents of, treats button-only navigation
as a designed case, and directly cures every item in the
owner's "raw" list (AA fonts, widgets, focus, animations,
text layout). Qt proper has no platform under us; Ultralite
is a commercial product for other people's silicon. The one
genuinely open question is flash size vs the boot partition —
which is why the plan starts with a measurement phase, not a
migration.

### Phased migration sketch (each phase: build + QEMU capture per `docs/VERIFIKASI-GUI.md` + serial transcript, as with all GUI work)

- **UI-1 — Vendor + port + measure (no visible change).**
  Vendor LVGL 9.5 under `user/uiapp/lvgl/` with a trimmed
  `lv_conf.h`; build block in `build-md.sh` next to the
  littlefs pattern; libc shim; port file (flush -> SYS_DISPLAY_FLUSH,
  tick -> `sys_uptime`, keypad indev -> event queue). A hidden
  smoke screen proves flush + focus + one AA label in QEMU.
  **Exit criteria: `llvm-size` numbers for trimmed LVGL and a
  written go/no-go on the boot-partition math** (trim harder,
  move daemon images to littlefs, or grow the partition —
  decide with data).
- **UI-2 — Shell + status bar + menu (proof screen).** LVGL
  main loop replaces the 33 ms repaint loop; status bar on
  the top layer (symbol icons, 1 s clock timer); menu rebuilt
  as LVGL tiles with group focus. Old screens unreachable
  from here on; face flow (grant/finale/menu) re-verified
  end to end with captures.
- **UI-3 — Settings subtree.** Settings list; BLE/LLM/Passkey
  screens; Wi-Fi list + the password-entry decision
  (LVGL keyboard vs kept char-picker) made from a captured
  interaction test.
- **UI-4 — Monitor + Power.** Chart/arc gauges on a data
  timer; power list wired to the existing sleep/reset
  syscalls.
- **UI-5 — Text edit + polish.** Textarea/picker final form;
  screen-load animations and focus styling tuned to the
  approved mock (palette already exists as C_* constants ->
  theme); idle-return and token-loss behaviour re-verified.
- **UI-6 — Cleanup + audit.** Delete replaced `ui_draw`
  screens, keep `ui_draw` only if face-adjacent helpers need
  it; publish final size table (uiapp image, kernel raw,
  headroom); full regression battery (SHELL-VERIFY, ONB1,
  ONB2, soak quick pass) green.

## 8. Honest costs (what we lose / pay)

- **Binary size growth is the price of admission**: uiapp
  61.5 KB -> [est] 250–400 KB, paid from a boot partition with
  ~291 KB free. UI-1 exists precisely because this might not
  close without a layout decision (daemon images out of the
  kernel image is the cleanest structural fix and benefits
  every daemon, but it is kernel work in its own right).
- **RAM**: +[est] 60–180 KB runtime. Against 64 MB: a
  non-event, stated plainly so nobody optimises the wrong
  budget.
- **SPI bandwidth**: invalidation *reduces* steady-state
  traffic vs today's full repaint every 33 ms (a selection
  move flushes two tiles, not 115 KB). But first paints and
  transitions still move large areas, animations run at the
  refresh period, and none of it is measurable in QEMU —
  pacing risk is deferred to hardware bring-up, same as the
  face already accepts.
- **The strip model**: we trade the deterministic
  "10 strips, always" flush for arbitrary invalidated
  rectangles. The ABI already supports rectangles; the HW
  SPI path will need windowed writes (standard ST7789
  CASET/RASET) instead of strip streaming.
- **Control**: ~100k+ LOC of upstream between our code and
  the pixels. When a frame is wrong, we will sometimes be
  debugging LVGL configuration and theme inheritance instead
  of 20 lines of our own renderer. Pin version 9.5; treat
  upgrades as projects.
- **Two UI technologies coexist** (face custom, uiapp LVGL):
  accepted deliberately (§3.7), but it is a real split a
  future contributor must understand.

## 9. Risks / unknowns only a spike can settle

1. **Trimmed LVGL `.text` with our clang/freestanding flags
   and our exact widget/font set** — decides the boot
   partition question. (Spike: link the UI-1 smoke uiapp,
   read `llvm-size`, compare against 291 KB headroom.)
2. **Freestanding friction**: which libc symbols LVGL
   actually pulls in our configuration; any float-formatting,
   `errno`, or compiler-runtime surprises under our clang
   flags (the kernel side already carries compiler-rt
   builtins for the TLS stack, so precedent exists).
3. **CPU cost of AA rendering + transitions on one A7 core**
   at 240x240 — measurable in QEMU in wall-clock terms, but
   QEMU timing is not RV1103 timing; treat as indicative.
4. **Wi-Fi password entry UX with buttons only** (LVGL
   keyboard + group focus vs our char picker) — needs a
   hands-on interaction capture, not a document.
5. **LVGL timer loop vs display-token ownership**: LVGL state
   must freeze cleanly while the face owns the display and
   resume on re-grant, including mid-animation; the
   acquire/release seam needs one deliberate test.

## 10. Sources

Local: `user/uiapp/{ui.c,ui_draw.c,ui_draw.h,main.c,screen.h,scr_*.c}`,
`user/face/{main.c,face_draw.c,face.h,qr.c}`, `user/ulib/ulib.h`,
`kernel/build-md.sh`, `kernel/kernel/arm/draft/spi0.c`,
`scripts/update-img/parameter.txt`, sizes via `llvm-size-18`
on the 2026-10-23 build.

Web (retrieved 2026-10-23):
- LVGL features/requirements + MIT license: https://lvgl.io/features
- LVGL v9.5 (ESP component registry README — simple-UI flash/RAM figures): https://components.espressif.com/components/lvgl/lvgl/versions/9.5.0/readme
- LVGL v9.5 release: https://lvgl.io/blog/release-v9-5
- LVGL display porting (flush_cb, partial buffers): https://lvgl.io/docs/open/9.2/porting/display
- LVGL input devices (keypad/encoder/groups, button-driven encoder): https://lvgl.io/docs/open/9.2/porting/indev
- LVGL fonts (built-in Montserrat bpp4, symbols): https://lvgl.io/docs/open/9.0/overview/font
- LVGL/SquareLine collaboration end (forum announcements): https://forum.lvgl.io/c/announcements/11
- SquareLine tier limits (third-party report of pricing): https://www.haraldkreuzer.net/en/news/dramatic-price-increase-squareline-studio
- Qt for MCUs docs (commercial license statement): https://doc.qt.io/archives/QtForMCUs-2.8/
- Qt Quick Ultralite minimum hardware requirements: https://doc.qt.io/archives/QtForMCUs-2.3/qtul-overview.html
- Qt Quick Ultralite performance logging (minimal example section sizes): https://doc.qt.io/QtForMCUs-2.9/qtul-performance-logging.html
- Qt for MCUs supported platforms/OS (bare metal, FreeRTOS): https://doc.qt.io/QtForMCUs-2.6/qtul-supported-platforms.html
- Qt device-creation pricing model (third-party, 2023): https://www.lenewt.com/en/blog/qt-open-source-embedded-systems/
- Nuklear (public domain, single header): https://socket.dev/go/package/github.com/immediate-mode-ui/nuklear
- emWin licensing (one-time, royalty-free): https://www.segger.com/products/user-interface/emwin/
- U8g2 (monochrome library reference): https://github-wiki-see.page/m/saviourxx/u8g2/wiki/u8g2reference
