/*
 * uartproto.h - link-layer client API, QaonicOS <-> co-MCU (C6L-1).
 *
 * Wire (frozen v1.1 / v2, see
 * docs/rencana-komunikasi-c6/WIRE-FREEZE.md):
 *   text: "CMD arg..\n" -> "OK data.." / "ERR kode pesan".
 *   multi-line text responses (WIFI.SCAN, WIFI.LIST) are
 *   count-prefixed: first line "OK <n>", then exactly n lines.
 *   KV values are length-prefixed (see uproto_kv_* below),
 *   never inline: values may contain spaces.
 *   binary frames: [magic(2B "QA") type(1B) len(2B BE)
 *   payload crc(2B BE)], CRC-16-CCITT (poly 0x1021,
 *   init 0xFFFF) over header+payload. Types: AUD 0x01,
 *   BIN 0x02, EVT 0x03. The legacy 3-byte "AUD" magic is
 *   rejected since C6L-1.
 *
 * Transport: the link layer talks to the co-MCU only
 * through a backend (uartproto/link.h). Default backend
 * is the in-process mock (QEMU); the bridge backend
 * (C6L-2) and the UART4 hardware backend use the same
 * interface, so client code does not change per backend.
 */
#ifndef UARTPROTO_H
#define UARTPROTO_H

/* Send cmd (without '\n'), read one response.
 * resp holds the payload after "OK " (no trailing
 * newline), always NUL-terminated. Returns 0 on OK,
 * -1 on ERR/timeout/malformed response. Multi-line
 * responses are returned whole (lines separated by
 * '\n'); the caller splits them. The response is
 * streamed into the caller's buffer: the link layer
 * imposes no fixed staging cap, only the caller's rsz
 * bounds it (excess bytes are dropped, result still 0
 * for text commands -- callers that need full lists
 * must pass a big enough buffer). */
int	uproto_cmd(const char *cmd, char *resp, unsigned rsz);

/* K5: uproto_cmd with a caller-supplied RPC timeout
 * (background poll use: a hung peer must not stall
 * the caller for the default 30 s). Implemented by
 * the linkd client stub (linkcli) only. */
int	uproto_cmd_t(const char *cmd, char *resp, unsigned rsz,
		     unsigned timeout_ms);

/* "CMD <arg>" (one argument, no spaces in arg). */
int	uproto_cmd1(const char *cmd, const char *arg, char *resp,
		    unsigned rsz);

/* "CMD <arg1> <arg2>". */
int	uproto_cmd2(const char *cmd, const char *a1, const char *a2,
		    char *resp, unsigned rsz);

/* --- KV: length-prefixed values (frozen v1.1) ---
 *
 * Wire:  KV.SET <key> <len>\n + exactly <len> value bytes
 *        -> "OK"
 *        KV.GET <key>\n -> "OK <len>\n" + exactly <len>
 *        value bytes (no trailing newline; the length
 *        IS the terminator)
 *        KV.DEL <key>\n -> "OK"
 * The store cap below is this implementation's NVS
 * limit (== sizeof(nvs_kv.val), user/cfg/cfg.h); the
 * framing itself supports arbitrary lengths. */
#define	UPROTO_KV_VAL_MAX	64u

/* Store a value (may contain spaces). Returns 0, or -1
 * if vlen exceeds UPROTO_KV_VAL_MAX, the key is empty,
 * or the peer reports an error. */
int	uproto_kv_set(const char *key, const char *val, unsigned vlen);

/* Fetch a value. val is NUL-terminated on return.
 * Returns 0 when the whole value fit, -2 when it was
 * truncated to vsz-1 bytes (the truncated bytes are
 * still written, so existing callers keep their old
 * behaviour), -1 on error / NOTFOUND. *vlen_out, when
 * non-NULL, receives the peer's full length. */
int	uproto_kv_get(const char *key, char *val, unsigned vsz,
		      unsigned *vlen_out);

/* Delete a key. Returns 0, or -1 on error. */
int	uproto_kv_del(const char *key);

/* --- NET.HTTP (C6L-2 peer addition, NOT frozen v1.1) ---
 * Wire: NET.HTTP GET <url>\n -> "OK <len>\n" + <len>
 * body bytes (KV.GET body shape). Returns 0 when the
 * whole body fit, -2 when truncated into body
 * (*len_out = the peer's full length), -1 on error. */
int	uproto_nethttp_get(const char *url, unsigned char *body,
			   unsigned cap, unsigned *len_out);

/* --- Binary frame format v2 (frozen) --- */
#define	LINK_FRAME_MAGIC0	'Q'
#define	LINK_FRAME_MAGIC1	'A'
#define	LINK_FRAME_TYPE_AUD	0x01u
#define	LINK_FRAME_TYPE_BIN	0x02u
#define	LINK_FRAME_TYPE_EVT	0x03u
#define	LINK_FRAME_HDR		5u	/* magic(2) + type(1) + len(2) */
#define	LINK_FRAME_CRC		2u
#define	LINK_MAX_PAYLOAD	1024u

unsigned	voice_crc16(const unsigned char *p, unsigned len);

/* VOICE.START <rate> <bits> -> 0, or -1. */
int	uproto_voice_start(unsigned rate, unsigned bits);
/* VOICE.STOP -> number of frames accepted by the co-MCU, or -1. */
int	uproto_voice_stop(void);
/* Send one AUD frame (PCM16 LE payload). Returns 0/-1. */
int	uproto_aud_frame(const unsigned char *pcm, unsigned len);

/* --- Inbound units: EVT + peer-initiated DISPATCH (C6L-3, wire
 * v1.2, see docs/rencana-komunikasi-c6/WIRE-FREEZE-V1.2.md) ---
 *
 * The stream mixes text lines and binary frames. EVT frames
 * (type 0x03) and peer DISPATCH lines can arrive at any time
 * -- between commands, or in the middle of a multi-line
 * response -- and the link layer queues them instead of
 * losing them: a queued unit survives a backend reopen
 * (units are application-level, not transport state), and
 * the consumer drains them with uproto_poll_inbound().
 * BIN data frames are NOT queued: WS data must be
 * consumed promptly by uproto_ws_recv(); a BIN frame
 * arriving during a text wait is a link-reset error.
 * A text line that is not a DISPATCH during a poll is
 * likewise a link-reset error (strict request/response:
 * the peer never sends unsolicited text).
 *
 * Queue bounds (v1.2): 4 units. A unit arriving at a full
 * queue is dropped and counted (uproto_inb_dropped());
 * the in-flight command is never failed for a full
 * queue. EVT payloads are capped at 256 bytes, DISPATCH
 * argument bodies at 512 bytes; anything larger is a
 * link-reset error at parse time. */
#define	UPROTO_INB_EVT		1u
#define	UPROTO_INB_DISPATCH	2u
#define	UPROTO_INB_EVT_MAX	256u
#define	UPROTO_INB_DISPATCH_ARGS_MAX	512u
#define	UPROTO_INB_TOOL_MAX	31u

struct uproto_inb {
	unsigned	kind;	/* EVT or DISPATCH */
	unsigned	id;	/* DISPATCH only */
	char		tool[UPROTO_INB_TOOL_MAX + 1];
				/* DISPATCH only */
	unsigned char	payload[UPROTO_INB_DISPATCH_ARGS_MAX];
				/* EVT line, or DISPATCH args JSON */
	unsigned	len;	/* payload length */
};

/* Return the next inbound unit, draining the queue
 * first, then waiting up to budget_ms of silence for a
 * fresh unit. Returns 1 (unit filled), 0 (nothing
 * within budget), -1 (link error: bad CRC, oversize
 * unit, stray text, dead link). Yields between
 * attempts so the network pump and the watchdog stay
 * fed. The caller owns the unit's meaning: EVT payload
 * is an event line ("WIFI.DROPPED"); DISPATCH is a
 * tool call the consumer must answer with
 * uproto_dispatch_result(). */
int	uproto_poll_inbound(struct uproto_inb *inb, unsigned budget_ms);

/* Drop all queued inbound units (fresh phase). Does
 * NOT touch the byte stream or the backend. */
void	uproto_inb_flush(void);

/* Number of inbound units dropped at a full queue
 * since boot (0 in normal operation). */
unsigned	uproto_inb_dropped(void);

/* --- TOOL.DISPATCH: guest-originated tool call (C6L-3) ---
 *
 * Wire: TOOL.DISPATCH <tool> <len>\n<args-json> ->
 *   "OK <len>\n<result-json>" or "ERR <CODE>".
 * The result body is JSON: {"ok":true,"result":...} on
 * tool success, {"ok":false,"error":"..."} on tool
 * failure (the dispatch itself succeeded). Unknown
 * tool is a wire error, not a JSON error: ERR NOTOOL.
 * args_json is capped at 128 bytes (the peers' body
 * cap for dispatch commands); result must fit rsz
 * (over-long results are consumed and reported as
 * TRUNC, never delivered partial).
 * Returns 0 (result filled, NUL-terminated), -1 (wire
 * error/timeout/TRUNC; err = "TIMEOUT"/"WIRE"/
 * "TRUNC" when esz allows), -2 (peer ERR; err = the
 * wire code, e.g. "NOTOOL"/"ARG"). */
int	uproto_tool_dispatch(const char *tool, const char *args_json,
			     char *result, unsigned rsz,
			     char *err, unsigned esz);

/* --- Peer-initiated DISPATCH: answer (C6L-3) ---
 *
 * Wire: DISPATCH.RESULT <id> <len>\n<result-json> ->
 * "OK". The result JSON uses the same shape as a
 * TOOL.DISPATCH result body. Returns 0/-1. */
int	uproto_dispatch_result(unsigned id, const char *result_json);

/* --- NET.WS: websocket-style framing over the data plane
 * (C6L-3, framing only; no product consumer yet) ---
 *
 * Wire: NET.WS.OPEN <url> -> "OK <sid>" (one session
 * at a time; a second OPEN while one is open is
 * ERR BUSY). Data rides BIN frames (type 0x02),
 * payload = the raw message (<= 1024 bytes); the
 * peer echoes the payload verbatim while the session
 * is open. NET.WS.CLOSE <sid> -> "OK" (no session or
 * wrong sid is ERR NOCONN). A BIN frame sent with no
 * open session is dropped by the peer (frames get no
 * response); the guest send reports -1. */
int	uproto_ws_open(const char *url, unsigned *sid_out);
/* Send one WS data message. Returns 0/-1. */
int	uproto_ws_send(const unsigned char *msg, unsigned len);
/* Receive one WS data message (echo). Returns 0 when
 * the whole message fit, -2 when truncated into buf
 * (*len_out = the peer's full length; the full frame
 * was consumed), -1 on error/timeout. */
int	uproto_ws_recv(unsigned char *buf, unsigned cap,
		       unsigned *len_out);
/* Close a WS session. Returns 0/-1. */
int	uproto_ws_close(unsigned sid);

/* --- Link state, counters, retry policy (C6L-4) ---
 *
 * Error semantics, as implemented (not just
 * documented): every command wrapper classifies its
 * outcome exactly once. A peer ERR answer counts as
 * a peer error and leaves the link state alone (the
 * peer answered; the link is fine). Any transport
 * failure -- timeout (silence or stall past
 * UPROTO_CMD_TIMEOUT_MS), dead link (peer FIN/RST),
 * bad frame CRC, oversize declared length, malformed
 * line -- marks the link DOWN with its reason and
 * resets the transport (backend close + byte
 * staging cleared; queued inbound units survive).
 * The next command lazily reopens the backend: that
 * is the auto-recover. DOWN clears on the next
 * successful command. Frame-send failure marks DOWN
 * (DEAD) and closes the backend without clearing
 * staging (no exchange is in flight).
 *
 * Retry policy: plain wrappers NEVER retry. The
 * _retry variants retry at most ONCE, only after a
 * TRANSPORT failure (never after a peer ERR), and
 * only for idempotent commands, per
 * uproto_retry_policy(): the allowlisted line
 * commands (WIFI.SCAN/LIST/STATUS/DISC, BLE.STATUS/
 * ON/OFF, TIME.GET, FIDO.LIST) unconditionally, and
 * KV.SET only when the caller asserts idem != 0
 * (same value re-written). WIFI.PASS, KV.SET with a
 * changed value, TOOL.DISPATCH, VOICE.*, NET.WS.*,
 * DISPATCH.RESULT, BLE.NAME, WIFI.CONNECT, and
 * unknown tokens are NEVER retried. KV.GET has no
 * retry variant: its length-prefixed body must be
 * read by uproto_kv_get, never by the line reader.
 * Counters are the observable: retries performed
 * vs denied, per-reason failure counts, resets. */
#define	UPROTO_LINK_UP		0
#define	UPROTO_LINK_DOWN	1
#define	UPROTO_LINK_REASON_NONE		0u
#define	UPROTO_LINK_REASON_TIMEOUT	1u
#define	UPROTO_LINK_REASON_DEAD		2u
#define	UPROTO_LINK_REASON_CRC		3u
#define	UPROTO_LINK_REASON_OVERSIZE	4u
#define	UPROTO_LINK_REASON_MALFORMED	5u

/* 0 = UP, 1 = DOWN. */
int	uproto_link_state(void);
/* Reason of the current DOWN state (NONE when UP). */
unsigned	uproto_link_reason(void);
/* All counters, in this order; any pointer may be
 * NULL to skip that counter. */
void	uproto_link_counters(unsigned *ok, unsigned *peer_err,
			     unsigned *timeout, unsigned *dead,
			     unsigned *crc_bad, unsigned *oversize,
			     unsigned *malformed, unsigned *resets,
			     unsigned *retries, unsigned *retry_denied,
			     unsigned *frames_sent,
			     unsigned *frames_send_fail);
/* 1 if cmd may be retried after a transport failure
 * (idem matters only for KV.SET), else 0. Pure
 * classifier: no I/O, safe for linkd (C6L-5) to reuse. */
int	uproto_retry_policy(const char *cmd, int idem);
/* Line command with the retry policy (see above).
 * cmd is one line without '\n'. Returns 0/-1 like
 * uproto_cmd. */
int	uproto_cmd_retry(const char *cmd, char *resp, unsigned rsz,
			 int idem);
/* KV.SET with the retry policy (idem != 0 asserts
 * the value is unchanged). Returns 0/-1 like
 * uproto_kv_set. */
int	uproto_kv_set_retry(const char *key, const char *val,
			    unsigned vlen, int idem);

/* --- Test-only mock inspection (C6L-3) ---
 * Implemented by the in-process mock backend only
 * (mock_comcu.c). The guest test reads back the
 * DISPATCH.RESULT receipts the mock captured, to
 * prove the peer received the guest's answers. */
unsigned	mock_comcu_script_result_count(void);
/* idx-th receipt: id + result JSON (NUL-terminated).
 * Returns 0/-1 (no such receipt). */
int	mock_comcu_script_result(unsigned idx, unsigned *id_out,
				 char *buf, unsigned cap);

#endif /* UARTPROTO_H */
