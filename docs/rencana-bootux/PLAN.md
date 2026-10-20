# PLAN -- Boot UX: Splash, Boot Modes, Dashboard Launch

Status: ACTIVE (2026-10-20, direction from Hafidz after the
first boot-video review)
Scope: what the user SEES from power-on, and how the shell and
the GUI dashboard relate. Dev build first; user mode designed
here, implemented next.

## Problems this solves (observed in the 2026-10-20 boot video)
1. The display is BLACK for the whole kernel boot: the virtio-gpu
   scanout is set in gpu_init() but the backing stays zeroed
   until the face daemon's first flush, seconds later.
2. The boot sequence feels arbitrary: face appears, switches to
   its waveform state, while the shell lives on serial -- no
   visible narrative of what state the device is in.
3. Everything autostarts at once. A normal OS boots to ONE
   surface; the shell is a developer/recovery tool, not a peer
   of the dashboard.

## Target experience

### Dev mode (this build -- implement now)
```
power on
  -> splash: OS logo (drawn graphic, not ASCII) + progress bar
  -> splash holds as the "device is up" idle screen
  -> serial shell qaon> is the working surface
  -> `qfetch` etc. as today
  -> shell command `gui` starts the dashboard (face + uiapp +
     qabotd) on demand; the display switches to the face
```

### User mode (design -- implement next)
```
power on
  -> splash: OS logo + progress
  -> GUI dashboard (face = home) directly; no shell anywhere
Shell entry (deliberately out-of-band, like a normal OS):
  -> physical button combination held during boot, or
  -> USB: a magic command on the USB serial gadget, or
  -> "USB debugging" toggle inside the GUI settings (A4),
     which enables the serial shell until toggled off / reboot
Mode selection: bootstate flag (persistent), set from the dev
shell (`setup mode dev|user`); default = dev until user mode
is complete and verified, then default flips to user.
```

## Work items
- [x] UX1 Kernel splash: gpu.c draws into the backing buffer
      right after SET_SCANOUT and flushes: logo (procedural Q
      ring + core, palette-matched to the face), "QaonicOS"
      wordmark (embedded 8x8 bitmap font, public-domain style),
      progress bar + stage label. Milestones update the bar:
      kernel init 25%, one-shots 60%, network 80%, ready 100%.
      Splash must no-op cleanly when no GPU (headless soak and
      HW SPI path unaffected).
- [x] UX2 Deferred daemon launch: daemons face/uiapp/qabotd no
      longer register at boot in dev mode; only sh does. New
      syscall SYS_GUI_START sets a pending flag; the scheduler
      loop performs launch_daemon for the GUI set from its own
      context (same machinery as boot-time launch). Shell
      builtin `gui` -> syscall -> "GUI starting" message.
      Refuse in safe mode (bootstate_safe_mode).
- [x] UX3 Mode flag groundwork: bootstate flags bit2 =
      user-mode (GUI autostart, no sh). Parsing at boot exists;
      the dev build still defaults to dev. (Shell entry
      mechanisms for user mode = next phase, see above.)
- [x] UX4 Tooling: capture-gui.sh drives the new flow (serial
      socket + `gui` command); record-boot-video.py captures
      logo -> shell -> qfetch -> gui -> face. Capture regex for
      qfetch must anchor on the prompt at line start (the old
      one truncated output at the "qaon> " inside the Shell
      line -- tooling bug, not an OS bug).

## Explicitly NOT in this phase
- Full framebuffer text console (fbcon) mirroring kernel
  printf. The splash progress indicator covers the "not black"
  requirement; fbcon stays an option if boot text on-device is
  ever needed for field diagnostics.
- The physical-button / USB shell-entry mechanisms (they need
  the hardware + USB gadget work; designed above).
- SPI display splash (the HW path reuses the same drawing code
  later via the display server; the kernel splash targets the
  virtio-gpu scanout used in QEMU).

## Verification
- All verify scripts (SHELL, SAFEMODE, FIRSTBOOT, UPDATE) PASS.
- New boot video: logo visible within the first frames, shell,
  qfetch (full output), `gui` -> face.
- Soak unaffected (headless boots never touch the splash).

## Outcome (2026-10-20)
Implemented and verified the same day: kernel splash (logo +
progress milestones 10/60/85/100), dev-mode shell-first boot,
SYS_GUI_START (102) with the launch performed in
sched_yield_switch, shell `gui` builtin, `setup mode user|dev`
(bootstate flags bit2), capture-gui.sh + record-boot-video.py
on the new flow. All four verify scripts PASS; boot video
recorded (logo -> shell -> qfetch -> gui -> face).

Side findings worth remembering:
- The dev disk image reached SAFE MODE during this work: a
  test boot killed mid-uupdsig left a staged v250 package that
  the next boot legitimately confirmed, pinning cur_version at
  250 and making UUPDSIG fail every later boot (fixed-version
  test packages are state-fragile). UUPDSIG now accepts -2 when
  cur_version >= 250. Test one-shots must tolerate any
  persistent state they can themselves create.
- QEMU's virtio-gpu is a loadable module: after a VM restart
  it silently disappeared ("not a valid device model name")
  until QEMU_MODULE_DIR was set. toolchain/env.sh now exports
  it; record-boot-video.py sets it too.
