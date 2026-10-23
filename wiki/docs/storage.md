# Storage

Two filesystems with two different jobs, plus one piece of state
that deliberately lives on **neither**: littlefs on `/data` holds
the device's own state, FAT32 on `/sd` is exchange media, and the
boot-resilience records sit in raw sectors at the device tail so a
filesystem failure can never take the recovery story with it.

> **Key facts**
>
> - **littlefs v2.9.3**, vendored in `kernel/kernel/arm/littlefs/`; syscalls **93–97**.
> - **QEMU today:** RAM backend, 1 MB (256 × 4096 B) — config does **not** survive QEMU reboots. FND-4 fixes this.
> - **On hardware:** SPI NAND via the SFC draft driver; littlefs region = NAND **blocks 320–999**.
> - NAND semantics are preserved by the port in both backends (erase → `0xFF`).
> - Config lives in littlefs as `cfg-*` files; credentials are `cfg-*` files in QSEC encrypted form (see [Onboarding & Security](onboarding-security.md)).

## littlefs on `/data`

The port (`lfs_port.c`) presents littlefs over the block layer and
mounts on demand. Userspace reaches it through syscalls 93–97 and
the shell's `/data` commands (`ls` / `cat` / `put`). The ULFS
one-shot battery is the regression test.

Two allocator lessons are baked into the port (both found the hard
way during onboarding): the original 4 KB bump pool with a no-op
free leaked on every rewrite — it is now a first-fit free-list
with coalescing over a 32 KB pool; and static byte arenas holding
typed headers must be 8-byte aligned, or the allocator panics.

### Backends

| Backend | Where | Persistence | Purpose |
|---|---|---|---|
| RAM (today's QEMU default) | `lfs_port.c` | none — volatile per boot | Development; ships with a **power-cut simulator** the B1/B2 resilience tests depend on |
| Persistent blk region (FND-4) | reserved region of the QEMU blk device | survives reboots | Mirrors the hardware layout (NAND blocks 320–999 mapped to a sector range) so provisioning/credential/onboarding flows can be regression-tested across boots. Must not collide with the bootstate sector, bootlog ring (device tail) or the selftest scratch sector |
| SPI NAND via SFC (hardware) | `draft/sfc_nand.c` | real | Rockchip SFC @ `0xffac0000`; W25N01GV opcodes; on-die ECC. The most complete of the draft drivers — and the hardware phase's keystone, since NAND is the product's only built-in persistent storage |

Backend selection under FND-4: persistent by default for normal
boots; the RAM backend stays selectable for the tests that need
volatility.

## FAT32 on `/sd`

The block layer (`blk.c`, syscalls 50/51) plus the FAT32 driver
(syscalls 52–56: mkdir/write/read/delete/readdir) expose `/sd`. In
QEMU this is a virtio-blk image; on hardware it is the Mini B's
**TF socket on SDMMC** (`0xffaa0000`) — the SoC research
[corrected](hardware-soc.md) the older "Mini B has no slot"
assumption. The shell already treats `/sd` as the exchange
surface: update packages arrive here (see [Updates](updates.md)),
and spawn can load program images from it — a second, concrete
reason the filesystem verdict in the
[boundary rules](kernel-userspace-boundary.md) is KEEP-in-kernel.

One policy question rides with the future SDMMC driver: the
BootROM gives an SD card boot priority over NAND, so a card is
both a recovery feature and a provisioning hazard. The card policy
(insert/remove behaviour, boot-priority stance) is a userspace
decision to be made in SOC-3's plan.

## What lives where

| Data | Home | Notes |
|---|---|---|
| Device config (`cfg-name`, `cfg-tz`, …) | littlefs | written by the `ufirst` one-shot with defaults on first boot |
| Credentials (API key, PIN hash) | littlefs, QSEC-encrypted | AES-256-GCM; legacy plaintext auto-migrates |
| Provisioning secret (QR payload) | littlefs | doubles as device identity until ONB-3 separates them |
| Boot state + bootlog | raw tail sectors | outside any FS, by design |
| Update packages | `/sd` (FAT) or `/data` | staged via the verified path only ([Updates](updates.md)) |
| Mock co-MCU "NVS" (dev only) | SD sectors via `user/cfg` | a documented trust-boundary deviation of the QEMU mock: on hardware, the C6's NVS is the C6's own |

Sources:
[`docs/rencana-storage-littlefs/PLAN-B1.md`](../docs/rencana-storage-littlefs/PLAN-B1.md),
[`docs/rencana-fondasi-tanpa-board/PLAN.md`](../docs/rencana-fondasi-tanpa-board/PLAN.md) (§FND-4),
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§5.8),
[`docs/riset-soc-rv1103/BAGIAN-C-IO.md`](../docs/riset-soc-rv1103/BAGIAN-C-IO.md) (§5 SFC, §6 SDMMC).
