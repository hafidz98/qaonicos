/*
 * linkd.h - LINKD-IPC v1: the client contract for linkd, the
 * single transport owner (C6L-5).
 *
 * Guest clients (uiapp screens, voice clients, the shell
 * `link` command, test programs) reach the C6 link ONLY
 * through linkd, over frozen uipc.h v1. They never link
 * the transport backends themselves. linkd owns the
 * single backend (bridge in QEMU, UART on hardware),
 * serialises client requests onto the strictly
 * alternating wire, forwards peer-initiated dispatches
 * to qabotd over IPC name `tools`, and fans EVT frames
 * out to subscribed clients.
 *
 * Message shape (all bodies are raw bytes; multi-byte
 * values are native little-endian unsigned words --
 * IPC is guest-local, never wire, so endianness is not
 * a wire concern; prior art (uiptoolssrv) does the
 * same). A request body is:
 *   word 0: op (LINKD_OP_*)
 *   op-specific fields (see below)
 * A reply body is:
 *   word 0: status (LINKD_* below)
 *   op-specific payload (see below)
 * The reply message id echoes the request op (never
 * 71, the Mach send-once death notice id).
 *
 * RPC sizing rule (frozen uipc.h v1): the len passed
 * to uipc_rpc_user is BOTH the sent size and the
 * reply budget. A reply larger than len is E2BIG and
 * is LOST (the wrapper destroys the reply port). The
 * client stub therefore pads short requests to the
 * op's reply budget with zero bytes; linkd parses
 * strings by NUL termination within the body length
 * and ignores trailing zeros. Budgets per op are
 * listed below; exceeding a budget on the reply side
 * is a linkd bug, not a client error.
 *
 * Status codes (reply word 0):
 *   OK        the operation succeeded; payload as documented.
 *   PEER_ERR  the peer answered with a wire ERR (payload =
 *             the wire code, e.g. "NOTOOL"/"ARG"/"NOTFOUND",
 *             or "" when the direct helper does not carry
 *             the code -- uproto_cmd family returns -1 for
 *             every failure, so linkd cannot always know
 *             it; TOOL.DISPATCH always carries it).
 *   LINK_FAIL the transport failed (peer silence, dead
 *             link, bad CRC, oversize, malformed -- the
 *             C6L-4 classification); payload = the reason
 *             string ("TIMEOUT"/"DEAD"/"CRC"/"OVERSIZE"/
 *             "MALFORMED"/"WIRE").
 *   TRUNC     the result was over-long and was consumed,
 *             never delivered partial (KV/NET.HTTP house
 *             style, via the stub's buffer).
 *   TIMEOUT   EVT_WAIT with budget 0 and no queued event
 *             (an immediate answer, not a wait).
 *   DENIED    a _retry request the C6L-4 policy refuses
 *             (never retried: WIFI.PASS, KV.SET with a
 *             changed value, TOOL.DISPATCH, VOICE.*,
 *             NET.WS.*, DISPATCH.RESULT, BLE.NAME,
 *             WIFI.CONNECT); the wire was NOT touched.
 *   UNSUP     an operation linkd does not serve (WS family,
 *             DISPATCH.RESULT -- peer dispatches go to
 *             `tools`, never to clients).
 *   BUSY      the EVT waiter table is full (4 waiters).
 *
 * Ops and layouts (offsets in words unless noted):
 *
 * CMD (1): req [op, mode, line NUL]. mode 0 = plain
 *   (uproto_cmd, never retries), 1 = retry-if-policy
 *   (uproto_cmd_retry; linkd applies the C6L-4
 *   classifier, it does not invent a policy).
 *   resp [status, payload NUL] (the response text, or
 *   the reason/code string on failure).
 *   Reply budget 2048 (WIFI.SCAN/LIST multi-line
 *   responses are the large case).
 * KV_SET (2): req [op, mode, vlen, key NUL, value bytes].
 *   mode 0 = plain, 1 = retry-if-policy (idem asserted
 *   by the caller: same value re-written).
 *   resp [status]. Budget 128.
 * KV_GET (3): req [op, key NUL].
 *   resp [status, vlen, value bytes]. Budget 512
 *   (values are <= 64 by the store cap).
 * KV_DEL (4): req [op, key NUL]. resp [status].
 *   Budget 128.
 * TOOL_DISPATCH (5): req [op, tool NUL, args-json NUL].
 *   resp [status, result-or-code NUL]. Budget 1024.
 * NETHTTP_GET (6): req [op, url NUL].
 *   resp [status, len, body bytes]. Budget 2048
 *   (body is 1024 by the C6L-2 peer).
 * VOICE_START (7): req [op, rate, bits]. resp [status].
 *   Budget 128.
 * VOICE_STOP (8): req [op]. resp [status, frames].
 *   Budget 128.
 * AUD_FRAME (9): req [op, len, pcm bytes].
 *   resp [status]. Budget 128 (the reply is tiny;
 *   the request itself is len + 8 <= 1032).
 * EVT_WAIT (10): req [op, last_seq, budget_ms].
 *   resp [status, seq, event-line NUL]. See the EVT
 *   section below. Budget 512.
 * STATUS (11): req [op]. resp [status, state, reason,
 *   ok, peer_err, timeout, dead, crc_bad, oversize,
 *   malformed, resets, retries, retry_denied,
 *   frames_sent, frames_send_fail, inb_dropped,
 *   evt_dropped, evt_seq, waiters, qlimit].
 *   Budget 512. Read-only; used by the shell `link`
 *   command and by tests.
 * DISPATCH_RESULT (12), WS_OPEN (13), WS_SEND (14),
 * WS_RECV (15), WS_CLOSE (16): accepted and answered
 * [UNSUPPORTED] immediately; the wire is not touched.
 *
 * Envelope validation: linkd validates the envelope
 * (field lengths, NUL termination within the body,
 * caps below) BEFORE touching the wire. An envelope
 * violation is answered PEER_ERR "ARG" -- the same
 * code the peer would give for a malformed command --
 * without a wire exchange. Caps: command line 159
 * chars (mirroring the direct tx buffer truncation
 * -- the stub truncates the same way); tool/key/url
 * 63 chars; TOOL.DISPATCH args 192 bytes (the wire
 * cap); KV value 64 bytes (the store cap, enforced
 * by the link layer too); AUD payload 1024 bytes
 * (LINK_MAX_PAYLOAD); EVT payload 256 bytes.
 *
 * EVT fan-out (subscription shape, C6L-5 design):
 * linkd keeps a broadcast ring of the last 8 EVT
 * units (sequence numbers from 1, monotonically
 * increasing) and up to 4 parked EVT_WAIT waiters.
 * EVT_WAIT semantics:
 *   - If the ring holds an event with seq >
 *     last_seq, linkd replies immediately with the
 *     OLDEST such event (a waiter more than 8
 *     events behind gets the oldest held event --
 *     a sequence jump the client can detect; the
 *     jump is counted in evt_dropped).
 *   - Else, if budget_ms == 0, linkd replies
 *     immediately with TIMEOUT.
 *   - Else linkd parks the waiter (stores the
 *     send-once reply name + last_seq) and replies
 *     when the next event arrives. The waiter's RPC
 *     timeout is the client's own: if the client
 *     times out first, its reply port is destroyed
 *     and linkd drops the waiter on the next event
 *     (the failed reply is not an error); the event
 *     itself stays in the ring for the next poll.
 *   - Every parked waiter receives every event that
 *     arrives while it is parked (true fan-out);
 *     a waiter consumes exactly one event per wait
 *     call and must re-issue for the next.
 * A parked waiter is NOT a queued command: while
 * linkd waits for an event it still serves commands
 * (the waiter is stored state, not a blocked wire
 * exchange). Waiters survive a transport reopen
 * (they are application-level, like the inbound
 * queue); they do NOT survive linkd's death (the
 * client sees EPEER/EDEST and re-issues after
 * linkd respawns and re-registers `link`).
 *
 * Concurrency model: linkd is single-threaded and
 * serves one wire exchange at a time. Client
 * requests queue in linkd's IPC port (default
 * qlimit 5 -- the queue decision, with its voice-
 * rate measurement, is recorded in DEVLOG-C6L5);
 * a sender to a full queue gets EFULL immediately
 * (sends never block) and must decide its own
 * retry/drop policy -- for voice frames the honest
 * policy is drop + count. Each reply goes to the
 * requesting client's own send-once reply name,
 * so responses can never cross between clients,
 * even when a slow command holds the wire while
 * other requests queue behind it.
 *
 * Death semantics (IPC-3): if a client dies while
 * its request is queued or in flight, linkd still
 * executes the wire exchange (it cannot un-execute
 * it) and its reply to the dead send-once name
 * fails harmlessly; other clients are unaffected.
 * If linkd itself dies, FND-3 respawns it (budget
 * 2/boot) and it starts from scratch: it
 * re-registers `link` (generation bumps, per
 * IPC-4), re-selects the bridge backend, and starts
 * with an empty EVT ring and no waiters.
 * Clients see EDEST/EPEER transiently and recover
 * on their next call (the stub looks `link` up
 * fresh on every call and caches nothing).
 *
 * Watchdog: linkd parks in blocking IPC receive
 * with a 100 ms deadline and polls inbound units
 * between receives, so it never spins and never
 * parks longer than a deadline without a syscall
 * (the FND-3 yield-or-park rule). A transport
 * reopen attempt is throttled to at most one per
 * 2000 ms while the link is DOWN, so a dead peer
 * cannot make the idle loop burn open attempts;
 * commands still attempt their own lazy reopen
 * per C6L-4 (bounded open cost, never a hang).
 *
 * What linkd does NOT do (explicit non-goals):
 *   - No WS sessions through linkd (per-client WS
 *     state + BIN draining would need a second
 *     loop shape; WS stays direct-link only until
 *     a later phase designs it).
 *   - No policy decisions: peer dispatches are
 *     forwarded to `tools` verbatim (tool + args);
 *     linkd never executes, confirms, or refuses a
 *     peer tool itself. The C6L-3 policy mirror
 *     lives in the tools consumer (stub for tests,
 *     qabotd's real gate in production).
 *   - No protocol changes: this phase is topology
 *     only (PLAN.md C6L-5 non-goals). The wire is
 *     v1.1 + v1.2, untouched.
 */
#ifndef QAON_LINKD_H
#define QAON_LINKD_H

#define	LINKD_NAME		"link"

/* Ops (request word 0). */
#define	LINKD_OP_CMD		1u
#define	LINKD_OP_KV_SET		2u
#define	LINKD_OP_KV_GET		3u
#define	LINKD_OP_KV_DEL		4u
#define	LINKD_OP_TOOL_DISPATCH	5u
#define	LINKD_OP_NETHTTP_GET	6u
#define	LINKD_OP_VOICE_START	7u
#define	LINKD_OP_VOICE_STOP	8u
#define	LINKD_OP_AUD_FRAME	9u
#define	LINKD_OP_EVT_WAIT	10u
#define	LINKD_OP_STATUS		11u
#define	LINKD_OP_DISPATCH_RESULT	12u
#define	LINKD_OP_WS_OPEN		13u
#define	LINKD_OP_WS_SEND		14u
#define	LINKD_OP_WS_RECV		15u
#define	LINKD_OP_WS_CLOSE	16u

/* Reply status (reply word 0). */
#define	LINKD_OK			0u
#define	LINKD_PEER_ERR		1u
#define	LINKD_LINK_FAIL		2u
#define	LINKD_TRUNC		3u
#define	LINKD_TIMEOUT		4u
#define	LINKD_DENIED		5u
#define	LINKD_UNSUP		6u
#define	LINKD_BUSY		7u

/* Linkd loop shape (documented, not ABI). */
#define	LINKD_RECV_DEADLINE_MS	100u	/* IPC park slice */
#define	LINKD_OPEN_THROTTLE_MS	2000u	/* reopen attempts while DOWN */
#define	LINKD_DRAIN_MAX		8u	/* inbound units per loop turn */
#define	LINKD_EVT_RING_N	8u
#define	LINKD_WAITERS_MAX	4u
#define	LINKD_TOOLS_RPC_MS	5000u	/* peer-dispatch forward budget */

/* Client stub RPC timeouts (documented, not ABI). */
#define	LINKD_RPC_CMD_MS		30000u
#define	LINKD_RPC_FRAME_MS	10000u
#define	LINKD_RPC_STATUS_MS	5000u

/* Envelope caps (linkd validates before the wire). */
#define	LINKD_LINE_MAX		159u
#define	LINKD_FIELD_MAX		63u	/* tool/key/url */
#define	LINKD_DISPATCH_ARGS_MAX	192u	/* the wire cap */
#define	LINKD_KV_VAL_MAX		64u	/* the store cap */

/* Reply budgets (RPC len = request size AND reply
 * budget; the stub pads to these). */
#define	LINKD_BUDGET_CMD		2048u
#define	LINKD_BUDGET_KV_SET	192u	/* req up to 164: 24 hdr + 12 + 64 key + 64 val */
#define	LINKD_BUDGET_KV_GET	512u
#define	LINKD_BUDGET_KV_DEL	128u
#define	LINKD_BUDGET_TOOL	1024u
#define	LINKD_BUDGET_NETHTTP	2048u
#define	LINKD_BUDGET_VOICE	128u
#define	LINKD_BUDGET_FRAME	1088u	/* req up to 1056: 24 hdr + 8 + 1024 pcm (reply tiny,
					 * but the budget is BOTH send size and reply
					 * budget -- 128 silently truncated every frame
					 * at C6L-5 first run: peer aud_ok stayed 0) */
#define	LINKD_BUDGET_EVT	512u
#define	LINKD_BUDGET_STATUS	512u

#endif /* QAON_LINKD_H */
