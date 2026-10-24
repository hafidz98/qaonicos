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

#endif /* UARTPROTO_H */
