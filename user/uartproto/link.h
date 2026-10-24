/*
 * link.h - transport-agnostic link layer backend interface (C6L-1).
 *
 * The link layer (uartproto.c) talks to the co-MCU ONLY through this
 * interface. Protocol code never knows which backend is live:
 *
 *   mock   - in-process mock co-MCU (mock_comcu.c). Unit-test
 *            backend; the mock parses the exact wire bytes a real
 *            peer would see, so wire behaviour is testable here.
 *   bridge - host Python peer over the guest TCP stack (C6L-2).
 *   uart   - RV1103 UART4 hardware backend (with the board).
 *
 * Backend contract:
 *   open  - (re)initialise transport state. Does NOT reset the
 *           peer's stored state (NVS, pairing); it only drops
 *           in-flight parser/queue state, i.e. it resyncs a
 *           desynchronised stream.
 *   write - send exactly len bytes. Returns 0, or -1 on link
 *           error. A single write() call carries one complete
 *           request (line, or line + length-prefixed body).
 *   read  - read up to len bytes of response. Returns a byte
 *           count > 0, 0 = no data available yet (the link
 *           layer retries until its timeout), -1 = link dead.
 *           Reads are chunked by design (the mock returns at
 *           most 32 bytes per call) so the link layer's
 *           streaming reader is genuinely exercised.
 *   send_frame - send one binary frame (AUD/BIN/EVT, see
 *           uartproto.h). Returns 0, or -1 if the peer
 *           rejected it.
 *   close - drop in-flight state (same scope as open).
 *
 * The link layer owns timeouts: it loops on read() == 0 until
 * its deadline (UPROTO_CMD_TIMEOUT_MS) and then reports a
 * timeout. Backends never block.
 */
#ifndef LINK_H
#define LINK_H

struct uproto_backend {
	const char	*name;
	int		(*open)(void);
	int		(*write)(const unsigned char *buf, unsigned len);
	int		(*read)(unsigned char *buf, unsigned len);
	int		(*send_frame)(const unsigned char *frame,
				      unsigned flen);
	int		(*close)(void);
};

/* The in-process mock backend (mock_comcu.c). */
extern struct uproto_backend uproto_backend_mock;

/* The host-bridge backend (bridge.c, C6L-2): guest TCP
 * to the host Python peer at 10.0.2.2:18082. */
extern struct uproto_backend uproto_backend_bridge;

/* Select a backend; NULL selects the mock. The previous
 * backend is closed and the new one opened. */
void	uproto_backend_select(struct uproto_backend *be);

/* The currently selected backend (never NULL). */
struct uproto_backend *uproto_backend_current(void);

/* Default per-command timeout, ms. A peer that answers
 * nothing within this window is reported as a timeout;
 * recovery (reconnect / backend reopen) is the caller's
 * job, see the wire-freeze document. */
#define	UPROTO_CMD_TIMEOUT_MS	2000u

#endif /* LINK_H */
