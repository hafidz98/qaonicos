# Networking & the C6 Link

QaonicOS has two networking stories that must not be confused. In
QEMU the guest has its own small in-kernel TCP/IP stack — a
development surface. On hardware, networking **terminates at the
ESP32-C6**: the RV1103 talks to the C6 over a UART link, and the
C6 owns Wi-Fi, BLE, TLS termination and key residency. This page
covers both, plus the C6L track that turns today's mock seam into
the real link layer.

> **Key facts**
>
> - Guest stack: ARP/IPv4/ICMP/UDP/TCP in kernel (`netstack.c`, `tcp.c`), **single TCP connection**, static IP 10.0.2.15 under QEMU slirp — **no DHCP anywhere**; DNS lives in userspace (`ntp.c` carries its own parser).
> - There is **no network thread**: `net_pump()` runs one server iteration from the scheduler's two switch points, so the stack advances only when daemons yield or exit.
> - The TCP **server** side has no syscall surface at all; syscalls 73–77 expose only the single **client** connection.
> - The C6 link today is a **function call**: the uartproto client library calls an in-process mock co-MCU. No byte crosses a device, and there is no data-UART driver in the kernel yet (console PL011 only).
> - FIDO private keys never cross the UART — in either direction, ever.

## The guest stack as-is

| Piece | Files | Notes |
|---|---|---|
| Virtio-net driver | `net.c` | polled, no IRQ |
| Ethernet/ARP/IPv4/ICMP/UDP | `netstack.c` | UDP exposed via syscalls 71/72 |
| TCP | `tcp.c` | one client connection (qabotd's provider) + one server connection (HTTP), separate state machines |
| HTTP server + web UI | `http.c`, `webui.c` | dashboard `/` + `/metrics` public; `/login`, `/settings`, `/shell` session-gated — see [Onboarding & Security](onboarding-security.md) |
| Pump | `netmain.c` via `user.c` | called from `sched_yield_switch` and the exit switch only |

Known environment limit, documented in `docs/UTANG-TEKNIS.md` §1:
TLS keep-alive over QEMU slirp succeeds only ~75% of the time
(the failure is a slirp/host interaction, proven not to be a
`tcp.c` bug); the official pattern is `Connection: close`. On
hardware this whole question moves to the C6.

## The wire protocol (v1 text + v2 frames)

Source of truth: `docs/rencana-app-qaonicos/RENCANA.md` §4,
frozen — with two known drifts the C6L-1 phase must resolve
(below). Commands are text lines, `CMD` → `OK`/`ERR`:

| Family | Commands | Notes |
|---|---|---|
| Wi-Fi | `WIFI.SCAN`, `WIFI.CONNECT`, `WIFI.PASS`, `WIFI.LIST`, `WIFI.FORGET` | implemented in the mock; `WIFI.STATUS`/`WIFI.DISC` exist as mock extensions |
| BLE | `BLE.ON`, `BLE.OFF`, `BLE.NAME`, `BLE.STATUS` | implemented; STATUS payload drifts from spec (see below) |
| HTTP via C6 | `NET.HTTP` (chunked) | in the frozen spec; **absent** from mock, client and all callers today |
| Time | `TIME.GET` | ISO-8601; usable as an NTP alternative source |
| Key-value | `KV.SET`, `KV.GET`, `KV.DEL` | backed by the mock's NVS (in QEMU: on the RV1103's own SD card — a documented trust-boundary deviation) |
| FIDO | `FIDO.LIST` (stub: `OK 0`); `FIDO.DEL`, `FIDO.APPROVE`, `FIDO.DENY`, async `FIDO.EVENT` | mostly **absent** — no async event path exists anywhere yet |

**v2 (binary, from the D4 audio slice):** `VOICE.START` /
`VOICE.STOP` plus AUD frames — `"AUD"` magic, length u16 BE,
CRC-16-CCITT (poly 0x1021, init 0xFFFF) over magic+length+payload.
Verified by the UVOICE battery. Not yet present: `TOOL.DISPATCH`
(peer-initiated by definition — it *requires* the async path),
generic BIN/EVT frames, `NET.WS` framing.

**Two recorded wire drifts** (spec vs code): (i) KV values are
length-prefixed in the frozen spec but inline in the code (so
values can't contain spaces and are TX-buffer-capped); (ii) the
`BLE.STATUS` payload differs. C6L-1 resolves both, on paper and in
code, before any real peer is written.

## The C6L track (host-side, board-less)

The QEMU channel verdict from the research: a second PL011 is
**impossible** on the toolchain's QEMU `-M virt` (the DTB has
exactly one, and PL011 is not user-creatable), and virtio-console
was rejected (a new kernel driver for a QEMU-only device, zero
transfer to the RV1103). The chosen shape is a **transport-agnostic
link layer** formalising the existing seam, with three backends.

| Phase | Content |
|---|---|
| C6L-1 | Link-layer formalisation + wire freeze (resolve the two drifts; backend interface `open/write/read-with-timeout/close` behind `xchg()`) |
| C6L-2 | Host-bridge backend + Python mock C6 peer (`scripts/mock-c6.py`) over the guest's existing TCP stack (slirp host 10.0.2.2). The Python peer is the **executable spec** the C6 firmware will later be written against |
| C6L-3 | v2 completion: `TOOL.DISPATCH`, EVT frames, `NET.WS` framing — the async path |
| C6L-4 | AUD streaming integrity + error semantics at a simulated 921600 baud |
| C6L-5 | **linkd**: a dedicated daemon becomes the single transport owner |

Design decisions: **D-C6L-1** one link layer, pluggable backends
(mock / bridge / uart); **D-C6L-2** the host peer is the executable
spec; **D-C6L-3** freeze the wire before building peers;
**D-C6L-4** one owner of the transport — because the guest TCP
stack is single-connection, the bridge backend and qabotd's
Mode-A provider can never hold it at once; linkd arbitrates.

Hardware-only follow-ups (not board-less): the C6 firmware itself
(FW-1..3, owner-deferred), UART4 electrical bring-up, real-baud
integrity, BLE coexistence.

## The C6's role (why this MCU)

Chosen in the F5 research
([`docs/riset-esp32c6-companion/RINGKASAN.md`](../docs/riset-esp32c6-companion/RINGKASAN.md)):
Wi-Fi 6 + BLE 5, 512 KB SRAM, and a security story that fits key
residency. Voice needs **921600 baud** (the RV1103 UART reaches
4 Mbps; use RTS/CTS); the link is **UART4** (UART2 is the debug
console; only UART2/3/4 are exposed on the Mini). The microphone
stays on the RV1103 — the C6 carries network and trust, not audio.
One firmware serves all modes; note the threading research's
bandwidth law: at ~92 KB/s raw, the C6 is a *link and key*
partner, not a general compute co-processor (and it has no FPU).

Sources:
[`docs/riset-komunikasi-c6/RINGKASAN.md`](../docs/riset-komunikasi-c6/RINGKASAN.md),
[`docs/rencana-komunikasi-c6/PLAN.md`](../docs/rencana-komunikasi-c6/PLAN.md),
[`docs/ARSITEKTUR.md`](../docs/ARSITEKTUR.md) (§2 rows 18–22, §4.2),
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§5.2, §5.6).
