# Qabot Agent Runtime

Qabot is the agent that lives on the device: a small harness
(`qabotd`, daemon 2) running a ReAct loop over an LLM provider,
with every tool call passing a pre-execution policy gate. The face
is its embodiment — the home screen *is* Qabot — and the harness
speaks three connectivity modes that are provider choices behind
the same loop.

> **Key facts**
>
> - **Loop:** ReAct, `MAX_STEPS = 8`, 30 s budget per task.
> - **Protocol:** the model answers `TOOL:<name> k=v` to act, `FINAL:<text>` to finish.
> - **Policy gate:** ALLOW / CONFIRM / BLOCK, decided *before* execution. `gpio_write` is CONFIRM (physical); `self_destruct` is BLOCK.
> - **TLS:** TLS 1.2 via vendored mbedTLS in userspace, over the guest TCP syscalls 73–77 — the boundary rules' model citizen.
> - **Tool dispatch from the link** (C6L `TOOL.DISPATCH`) goes **only** through the gate, never around it.

## The harness

`user/qabotd.c` + the `user/qabot/` library (loop, policy, tools,
JSON, history). Inputs arrive from the console, from voice (AUD
frames over the link protocol), and — once C6L-3 lands — from
`TOOL.DISPATCH` over the C6 link. Outputs set the face state
(idle / listening / speaking / thinking via syscall 78) and reply
on console or web. The voice data plane exists today: the D4
audio slice (`VOICE.START`/`VOICE.STOP` + CRC-checked AUD frames)
is verified in QEMU, with the microphone on the RV1103 side
(ACODEC + I2S0 RX, syscalls 82–88).

The gate is the safety boundary of the whole product idea: an LLM
may *propose*, but anything that touches the physical world needs
a physical or web confirmation, and destructive actions are
refused outright. That split — pure-logic brain, thin platform
tools, gate in between — is also the shape the agent-runtime
research validated against external harnesses
([`docs/riset-agent-runtime-harness/`](../docs/riset-agent-runtime-harness/RINGKASAN.md)).

## Connectivity modes A / B / C

| Mode | Shape | Status |
|---|---|---|
| **A — direct** | The device calls an LLM API itself (OpenAI-compatible endpoint; Meta Model API at `https://api.meta.ai/v1` verified real) | Built for dev/QEMU; **live use parked** — the owner skipped the API key for now. On hardware, Mode A is expected to move behind C6 TLS termination |
| **B — companion** | The device is the *server* (MCP-style); a cloud-side harness is the client and Qabot is its companion. Interim companions: Jivix and the Muse Gadget | Design; lands with C6L-3 + the C6 firmware track. Private keys reside on the C6 |
| **C — hybrid** | A and B combined, per task/policy | Design |

One clarification the research established: there is no
"Muse SDK" product and Meta sells no harness-as-a-service — in
Mode B, the harness on the cloud side is effectively Jivix (the
owner's separate multi-platform harness project). The Muse
Gadget SDK itself is real and active (Apache-2.0, ESP32-C6 a
first-class target, pairing via the Muse app + physical press) —
it validates Qabot's Mode B reference design rather than being a
dependency.

## Competitor context (short)

The full landscape is
[`docs/riset-agent-bots/RINGKASAN.md`](../docs/riset-agent-bots/RINGKASAN.md):
16 consumer product families, plus a §7 reference catalog
(owner-supplied, josephzhou.me/ai) of 129 products / 54 brands.
The market lessons recorded there: Humane is dead (servers shut
Feb 2025), Limitless was acquired by Meta, Rabbit pivoted to
BYOK software, Vector's voice sits behind a subscription. At the
maker layer the voice terminal itself is a commodity ($8–60:
Atom Echo, ESP32-S3-BOX-3, Waveshare touch LCD, StackChan) — so
Qabot's differentiation is not the terminal. It is the OS
underneath it, the procedural face, the physical-confirmation UX,
and harness modes A/B/C.

Sources:
[`docs/rencana-qabot-harness/PLAN.md`](../docs/rencana-qabot-harness/PLAN.md),
[`PLAN-FONDASI.md`](../docs/rencana-qabot-harness/PLAN-FONDASI.md),
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§5.3),
[`docs/riset-agent-bots/RINGKASAN.md`](../docs/riset-agent-bots/RINGKASAN.md),
[`docs/riset-agent-runtime-harness/RINGKASAN.md`](../docs/riset-agent-runtime-harness/RINGKASAN.md).
