/*
 * user/ntp.c -- Daemon sinkron jam via NTP/DNS (App A4).
 *
 * Daemon ke-3 dalam scheduler kooperatif.  Jangan pernah exit dan
 * jangan sentuh syscall display (tidak pegang token).
 *
 * Alur: resolve "pool.ntp.org" via DNS ke 10.0.2.3:53 (DNS bawaan
 * QEMU user-net), lalu query SNTP ke server:123 dan set jam dinding
 * kernel via sys_time_set().  Sinkron saat boot, ulangi tiap 3600
 * detik.  Semua tunggu memakai pola non-blocking: recv -> yield.
 *
 * Bare-metal, tanpa libc: buffer statis, helper cetak sendiri.
 */
#include "ulib/ulib.h"

#define DNS_IP		0x0A000203u	/* 10.0.2.3, host order */
#define DNS_PORT	53u
#define NTP_PORT	123u
#define NTP_DELTA	2208988800u	/* detik 1900-01-01 -> 1970-01-01 */
#define MIN_UNIX	1700000000u	/* tolak jam < 2023 (tidak waras) */
#define TIMEOUT_MS	3000u		/* timeout per percobaan */
#define RETRY		3		/* jumlah percobaan DNS/NTP */
#define RESYNC_SEC	3600u		/* interval sinkron ulang */
#define FAIL_WAIT_SEC	60u		/* jeda bila gagal total */

/* ------------------------------------------------------------------ */
/* Helper cetak (tanpa libc).                                          */
/* ------------------------------------------------------------------ */

/* Tulis satu karakter ke console. */
static void
putc1(char c)
{
	sys_write(1, &c, 1u);
}

/* Cetak unsigned desimal. */
static void
putu(unsigned v)
{
	char tmp[11];
	int n = 0, i;

	if (v == 0u) {
		putc1('0');
		return;
	}
	while (v > 0u && n < 11) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	for (i = n - 1; i >= 0; i--)
		putc1(tmp[i]);
}

/* Cetak IPv4 host-order sebagai a.b.c.d. */
static void
putip(unsigned ip)
{
	putu((ip >> 24) & 0xFFu);
	putc1('.');
	putu((ip >> 16) & 0xFFu);
	putc1('.');
	putu((ip >> 8) & 0xFFu);
	putc1('.');
	putu(ip & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Util kecil.                                                         */
/* ------------------------------------------------------------------ */

/* Tidur ~sec detik: poll sys_uptime + yield (scheduler tetap jalan). */
static void
sleep_s(unsigned sec)
{
	unsigned t0 = sys_uptime();

	while (sys_uptime() - t0 < sec * 1000u)
		sys_yield();
}

/* ------------------------------------------------------------------ */
/* Klien DNS (A record).                                               */
/* ------------------------------------------------------------------ */

/* Lewati satu nama domain ter-encode di paket DNS.  Tangani pointer
 * kompresi (0xC0..).  Kembalikan offset setelah nama, atau >= n bila
 * paket rusak. */
static unsigned
dns_skip_name(const unsigned char *r, int n, unsigned off)
{
	unsigned len;

	for (;;) {
		if (off >= (unsigned)n)
			return (unsigned)n;
		len = r[off];
		if ((len & 0xC0u) == 0xC0u)
			return off + 2u;	/* pointer: 2 byte, nama selesai */
		if (len == 0u)
			return off + 1u;
		off += 1u + len;
	}
}

/* Parse respons DNS: cocokkan ID, lewati question, iterasi answer,
 * ambil RDATA dari jawaban TYPE=1 (A) pertama.  IP dikembalikan dalam
 * host order.  Kembalikan 0 bila dapat, -1 bila tidak. */
static int
dns_parse(const unsigned char *r, int n, unsigned id, unsigned *ip_out)
{
	unsigned qd, an, off, i, typ, rdlen;

	if (n < 12)
		return -1;
	if (((unsigned)r[0] << 8 | (unsigned)r[1]) != (id & 0xFFFFu))
		return -1;			/* bukan jawaban untuk query kita */
	if ((r[2] & 0x80u) == 0u)
		return -1;			/* bukan respons */
	qd = (unsigned)r[4] << 8 | (unsigned)r[5];
	an = (unsigned)r[6] << 8 | (unsigned)r[7];

	/* Lewati question. */
	off = 12u;
	for (i = 0u; i < qd; i++) {
		off = dns_skip_name(r, n, off);
		if (off + 4u > (unsigned)n)
			return -1;
		off += 4u;			/* QTYPE + QCLASS */
	}

	/* Iterasi answer. */
	for (i = 0u; i < an; i++) {
		off = dns_skip_name(r, n, off);
		if (off + 10u > (unsigned)n)
			return -1;
		typ = (unsigned)r[off] << 8 | (unsigned)r[off + 1u];
		/* class(2) + ttl(4) dilewati; rdlen di off+8..9 */
		rdlen = (unsigned)r[off + 8u] << 8 | (unsigned)r[off + 9u];
		off += 10u;
		if (off + rdlen > (unsigned)n)
			return -1;
		if (typ == 1u && rdlen == 4u) {
			/* network order -> host order */
			*ip_out = (unsigned)r[off] << 24 |
				  (unsigned)r[off + 1u] << 16 |
				  (unsigned)r[off + 2u] << 8 |
				  (unsigned)r[off + 3u];
			return 0;
		}
		off += rdlen;
	}
	return -1;
}

/* Resolve nama A record via DNS_IP:53.  Kembalikan 0 + IP (host order)
 * bila sukses, -1 bila gagal total. */
static int
dns_resolve(const char *name, unsigned *ip_out)
{
	static unsigned char q[64];
	static unsigned char r[512];
	unsigned qlen, off, id, t0, sip, i, k;
	unsigned short sport;
	const char *p;
	int att, sent, n;

	/* Bangun query (header 12 byte + QNAME + QTYPE/QCLASS). */
	off = 12u;
	q[2] = 0x01u; q[3] = 0x00u;	/* flags: recursion desired */
	q[4] = 0x00u; q[5] = 0x01u;	/* QDCOUNT = 1 */
	q[6] = 0u; q[7] = 0u;		/* ANCOUNT = 0 */
	q[8] = 0u; q[9] = 0u;		/* NSCOUNT = 0 */
	q[10] = 0u; q[11] = 0u;		/* ARCOUNT = 0 */

	/* Encode QNAME label per label ("pool.ntp.org"). */
	p = name;
	for (;;) {
		const char *lp = p;
		unsigned ln = 0u;

		while (*lp && *lp != '.') {
			ln++;
			lp++;
		}
		q[off++] = (unsigned char)ln;
		for (i = 0u; i < ln; i++)
			q[off++] = (unsigned char)p[i];
		p = lp;
		if (*p == 0)
			break;
		p++;			/* lewati '.' */
	}
	q[off++] = 0u;			/* terminator nama */
	q[off++] = 0u; q[off++] = 1u;	/* QTYPE = A */
	q[off++] = 0u; q[off++] = 1u;	/* QCLASS = IN */
	qlen = off;

	for (att = 0; att < RETRY; att++) {
		/* ID acak per percobaan (dari uptime; cukup untuk QEMU). */
		id = (sys_uptime() + (unsigned)att * 0x1F1Fu) & 0xFFFFu;
		q[0] = (unsigned char)(id >> 8);
		q[1] = (unsigned char)(id & 0xFFu);

		/* Kirim; tolerir -1 (MAC belum dikenal: kernel sudah
		 * kirim ARP, yield lalu coba lagi). */
		sent = -1;
		for (k = 0u; k < 20u; k++) {
			sent = sys_udp_send(DNS_IP, DNS_PORT, q, qlen);
			if (sent == 0)
				break;
			sys_yield();
		}
		if (sent != 0)
			continue;	/* gagal kirim: percobaan berikutnya */

		/* Tunggu respons: recv non-blocking + yield. */
		t0 = sys_uptime();
		for (;;) {
			n = sys_udp_recv(r, sizeof(r), &sip, &sport);
			if (n > 0) {
				if (dns_parse(r, n, id, ip_out) == 0)
					return 0;
				/* Paket basi/bukan jawaban kita: buang,
				 * lanjut tunggu. */
			}
			if (sys_uptime() - t0 >= TIMEOUT_MS)
				break;
			sys_yield();
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Klien SNTP.                                                         */
/* ------------------------------------------------------------------ */

/* Query SNTP ke server_ip:123.  Ambil transmit timestamp (byte 40-43),
 * konversi ke unix epoch, validasi, lalu sys_time_set().
 * Kembalikan 0 bila jam diset, -1 gagal total, -2 jam tidak waras. */
static int
sntp_sync(unsigned server_ip)
{
	static unsigned char q[48];
	static unsigned char r[512];
	unsigned t0, sip, ntp_sec, unix;
	unsigned short sport;
	int att, k, n, sent;

	for (k = 0; k < 48; k++)
		q[k] = 0;
	q[0] = 0x1Bu;	/* LI=0, VN=3, Mode=3 (client) */

	for (att = 0; att < RETRY; att++) {
		sent = -1;
		for (k = 0; k < 20; k++) {
			sent = sys_udp_send(server_ip, NTP_PORT, q, 48u);
			if (sent == 0)
				break;
			sys_yield();
		}
		if (sent != 0)
			continue;

		t0 = sys_uptime();
		for (;;) {
			n = sys_udp_recv(r, sizeof(r), &sip, &sport);
			if (n >= 48 && sip == server_ip) {
				/* Transmit timestamp, big-endian,
				 * detik sejak 1900-01-01. */
				ntp_sec = (unsigned)r[40] << 24 |
					  (unsigned)r[41] << 16 |
					  (unsigned)r[42] << 8 |
					  (unsigned)r[43];
				unix = ntp_sec - NTP_DELTA;
				if (unix < MIN_UNIX)
					return -2;	/* tolak jam aneh */
				sys_time_set(unix);
				return 0;
			}
			if (sys_uptime() - t0 >= TIMEOUT_MS)
				break;
			sys_yield();
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Daemon utama.                                                       */
/* ------------------------------------------------------------------ */

__attribute__((section(".text.start")))
void
_start(void)
{
	unsigned ip;
	int r;

	puts("ntp: daemon sinkron jam mulai\n");

	for (;;) {
		/* 1. Resolve pool.ntp.org. */
		if (dns_resolve("pool.ntp.org", &ip) != 0) {
			puts("ntp: gagal, coba lagi 60 dtk\n");
			sleep_s(FAIL_WAIT_SEC);
			continue;
		}
		puts("ntp: pool.ntp.org -> ");
		putip(ip);
		putc1('\n');

		/* 2. Query SNTP + set jam kernel. */
		r = sntp_sync(ip);
		if (r == 0) {
			puts("ntp: unix=");
			putu(sys_time_get());
			puts(" (diset)\n");
		} else if (r == -2) {
			puts("ntp: jam server tidak waras, coba lagi 60 dtk\n");
		} else {
			puts("ntp: gagal, coba lagi 60 dtk\n");
		}

		/* 3. Tidur hingga sinkron ulang (atau gagal -> tetap
		 * tunggu penuh; loop atas menangani retry 60 dtk
		 * hanya bila gagal total). */
		if (r == 0)
			sleep_s(RESYNC_SEC);
		else
			sleep_s(FAIL_WAIT_SEC);
	}
}
