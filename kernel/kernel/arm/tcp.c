/*
 * mach3/kernel/arm/tcp.c -- Server TCP minimal (satu koneksi, port 80)
 * untuk HTTP (Fase D).
 *
 * Port dari archive/kernel-scratch/kernel/src/tcp.c (Fase 12).
 * Batasan yang disengaja: satu koneksi dalam satu waktu, tanpa window
 * scaling/SACK/timestamps, MSS implisit 536 (kita tak kirim opsi).
 * Data request diakumulasi sampai "\r\n\r\n" lalu http_handle() dipanggil
 * dan respons dikirim sekaligus (HTTP/1.0, Connection: close).
 */

/* IP send (netstack.c). */
extern int	netstack_ip_send(unsigned int dst, unsigned char proto,
				  const unsigned char *payload,
				  unsigned paylen);

/* HTTP handler (http.c). */
extern unsigned	http_handle(const unsigned char *req, unsigned reqlen,
			    unsigned char *resp);

/* Timer (clock.c): ticks 100 Hz. */
extern unsigned int	arm_timer_ticks(void);
extern void	netstack_arp_learn(unsigned int ip, const unsigned char *mac);
extern int		printf(const char *, ...);

/* Port server HTTP. */
#define	TCP_PORT	80u

/* IP guest (dari netstack.c). */
#define	NET_IP		0x0A00020Fu

#define TF_FIN 0x01u
#define TF_SYN 0x02u
#define TF_RST 0x04u
#define TF_PSH 0x08u
#define TF_ACK 0x10u

enum { TS_LISTEN = 0, TS_SYN_RCVD, TS_ESTABLISHED, TS_FIN_SENT };

#define TCP_HDRLEN  20u
#define TCP_MAXSEG  1200u   /* payload max per segmen (aman < MTU) */
#define RTO_MS      800u
#define CONN_TO_MS  30000u

static unsigned short rd16(const unsigned char *p)
{
    return (unsigned short)((p[0] << 8) | p[1]);
}
static unsigned int rd32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | p[3];
}
static void wr16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xffu);
}
static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)(v);
}

/* Checksum TCP: pseudo-header + segmen. */
static unsigned short tcp_csum(unsigned int src, unsigned int dst,
                         const unsigned char *seg, unsigned seglen)
{
    unsigned int s = 0;
    unsigned i;

    s += (src >> 16) & 0xffffu;
    s += src & 0xffffu;
    s += (dst >> 16) & 0xffffu;
    s += dst & 0xffffu;
    s += 6u;                 /* protocol = TCP */
    s += seglen;
    for (i = 0; i + 1 < seglen; i += 2)
        s += (unsigned short)((seg[i] << 8) | seg[i + 1]);
    if (seglen & 1u)
        s += (unsigned short)(seg[seglen - 1] << 8);
    while (s >> 16)
        s = (s & 0xffffu) + (s >> 16);
    return (unsigned short)~s;
}

static struct {
    unsigned char  state;
    unsigned int rip;                 /* IP remote */
    unsigned short rport;               /* port remote */
    unsigned int iss;                 /* initial seq kita */
    unsigned int snd_nxt;             /* seq berikutnya untuk kirim */
    unsigned int snd_una;             /* seq tertua yang belum di-ack */
    unsigned int rcv_nxt;             /* seq berikutnya yang diharapkan */
    unsigned int last_act;            /* (arm_timer_ticks() * 10u) terakhir ada aktivitas */
    /* Segmen terakhir yang butuh ack (untuk retransmit): */
    unsigned char  last_seg[TCP_HDRLEN + TCP_MAXSEG];
    unsigned last_seglen;         /* panjang segmen TCP (header+payload) */
    unsigned char  last_flags;          /* flags segmen terakhir */
    unsigned int last_tx_ms;          /* kapan terakhir dikirim */
    unsigned char  last_valid;
    /* Buffer request HTTP: */
    unsigned char  req[1024];
    unsigned reqlen;
    unsigned char  responded;
    /* Respons HTTP yang sedang dikirim (untuk retransmit per-segmen): */
    const unsigned char *tx_base;  /* buffer respons (milik tcp_respond) */
    unsigned tx_len;         /* panjang respons */
    unsigned int tx_seq0;        /* seq number dari tx_base[0] */
    unsigned char  tx_active;      /* 1 selama respons belum di-ack semua */
} tc;

static unsigned st_rx, st_tx, st_conns;

/* Kirim segmen TCP. flags = kombinasi TF_*. payload boleh NULL/0.
 * Meng-update snd_nxt & menyimpan salinan untuk retransmit bila segmen
 * membawa SYN/FIN/data (pure ACK tak perlu retransmit). */
static void tcp_send(unsigned char flags, const unsigned char *payload, unsigned plen)
{
    unsigned char *s = tc.last_seg; /* bangun langsung di buffer retransmit */
    unsigned seglen = TCP_HDRLEN + plen;

    wr16(s + 0, TCP_PORT);
    wr16(s + 2, tc.rport);
    wr32(s + 4, tc.snd_nxt);
    wr32(s + 8, tc.rcv_nxt);
    s[12] = (unsigned char)(5u << 4);  /* data offset = 5 (20 byte), tanpa opsi */
    s[13] = flags;
    wr16(s + 14, 4096u);        /* window */
    wr16(s + 16, 0u);           /* checksum, dihitung di bawah */
    wr16(s + 18, 0u);           /* urgent */
    {
        unsigned i;
        for (i = 0; i < plen; i++)
            s[TCP_HDRLEN + i] = payload[i];
    }
    wr16(s + 16, tcp_csum(NET_IP, tc.rip, s, seglen));

    if (netstack_ip_send(tc.rip, 6u, s, seglen) == 0) {
        st_tx++;
        tc.snd_nxt += plen;
        if (flags & (TF_SYN | TF_FIN))
            tc.snd_nxt++;
        tc.last_seglen = seglen;
        tc.last_flags = flags;
        tc.last_tx_ms = (arm_timer_ticks() * 10u);
        /* Pure ACK tak perlu di-retransmit. */
        tc.last_valid = (flags & (TF_SYN | TF_FIN)) || plen > 0;
    }
}

/* ACK murni (tak mengubah snd_nxt, tak disimpan untuk retransmit). */
static void tcp_ack(void)
{
    unsigned char s[TCP_HDRLEN];

    wr16(s + 0, TCP_PORT);
    wr16(s + 2, tc.rport);
    wr32(s + 4, tc.snd_nxt);
    wr32(s + 8, tc.rcv_nxt);
    s[12] = (unsigned char)(5u << 4);
    s[13] = TF_ACK;
    wr16(s + 14, 4096u);
    wr16(s + 16, 0u);
    wr16(s + 18, 0u);
    wr16(s + 16, tcp_csum(NET_IP, tc.rip, s, TCP_HDRLEN));
    if (netstack_ip_send(tc.rip, 6u, s, TCP_HDRLEN) == 0)
        st_tx++;
}

static void tcp_reset_conn(void)
{
    tc.state = TS_LISTEN;
    tc.reqlen = 0;
    tc.responded = 0;
    tc.last_valid = 0;
    tc.tx_active = 0;
}

/* Kirim respons HTTP per-segmen lalu FIN (dipanggil sekali saat request
 * lengkap). HTTP/1.0, Connection: close. */
static void tcp_respond(void)
{
    static unsigned char resp[8704];
    unsigned rlen, off;

    tc.responded = 1;
    rlen = http_handle(tc.req, tc.reqlen, resp);
    if (rlen > sizeof(resp))
        rlen = sizeof(resp);
    /* Catat rentang data agar tcp_tick bisa retransmit per-segmen. */
    tc.tx_base = resp;
    tc.tx_len = rlen;
    tc.tx_seq0 = tc.snd_nxt;
    tc.tx_active = 1;
    off = 0;
    while (off < rlen) {
        unsigned n = rlen - off;
        if (n > TCP_MAXSEG)
            n = TCP_MAXSEG;
        tcp_send(TF_PSH | TF_ACK, resp + off, n);
        off += n;
    }
    /* Langsung FIN (HTTP/1.0 Connection: close). */
    tcp_send(TF_FIN | TF_ACK, 0, 0);
    tc.state = TS_FIN_SENT;
    printf("[tcp] respons dikirim, FIN\r\n");
}

static int req_complete(void)
{
    unsigned i;
    if (tc.reqlen < 4)
        return 0;
    for (i = 0; i + 3 < tc.reqlen; i++)
        if (tc.req[i] == '\r' && tc.req[i + 1] == '\n' &&
            tc.req[i + 2] == '\r' && tc.req[i + 3] == '\n')
            return 1;
    return 0;
}

/* Proses payload data yang valid (seq == rcv_nxt). */
static void tcp_on_data(unsigned int seq, const unsigned char *t,
                        unsigned doff, unsigned datalen)
{
    unsigned i;

    if (datalen == 0)
        return;
    if (seq == tc.rcv_nxt) {
        unsigned room = sizeof(tc.req) - tc.reqlen;
        unsigned take = datalen < room ? datalen : room;
        for (i = 0; i < take; i++)
            tc.req[tc.reqlen + i] = t[doff + i];
        tc.reqlen += take;
        tc.rcv_nxt += datalen;
        tcp_ack();
        if (!tc.responded && req_complete())
            tcp_respond();
    } else if (seq < tc.rcv_nxt) {
        tcp_ack();      /* duplikat: ack ulang */
    }
    /* seq > rcv_nxt: out-of-order, abaikan (tanpa SACK). */
}

/* =====================================================================
 * TCP client (Q2a): satu koneksi aktif, pola request/response.
 *
 * Batasan yang disengaja (cermin server): tanpa window scaling/SACK,
 * stop-and-wait per segmen (satu segmen in-flight), buffer RX 4KB.
 * Cukup untuk HTTPS API (kirim request, baca respons).
 * Server di atas (struct tc) tidak disentuh.
 * ===================================================================== */

enum { TCC_CLOSED = 0, TCC_SYN_SENT, TCC_ESTABLISHED, TCC_FIN_SENT };

#define TCC_RXLEN 4096u

static struct {
    unsigned char  state;
    unsigned int rip;                 /* IP remote */
    unsigned short rport;               /* port remote */
    unsigned short lport;               /* ephemeral lokal, basis 0xD000 */
    unsigned int iss;
    unsigned int snd_nxt;
    unsigned int snd_una;
    unsigned int rcv_nxt;
    unsigned int last_act;
    unsigned char  last_seg[TCP_HDRLEN + TCP_MAXSEG];
    unsigned last_seglen;
    unsigned char  last_flags;
    unsigned int last_tx_ms;
    unsigned char  last_valid;
    unsigned char  rx[TCC_RXLEN];        /* buffer terima */
    unsigned rxlen;
    unsigned char  peer_closed;
} tcc;

static unsigned short tcc_next_port = 0xD000u;

static unsigned tcc_now(void) { return arm_timer_ticks() * 10u; }

/* Kirim segmen via koneksi client (cermin tcp_send). */
static void tcc_send(unsigned char flags, const unsigned char *payload,
                     unsigned plen)
{
    unsigned char *s = tcc.last_seg;
    unsigned seglen = TCP_HDRLEN + plen;
    unsigned i;

    wr16(s + 0, tcc.lport);
    wr16(s + 2, tcc.rport);
    wr32(s + 4, tcc.snd_nxt);
    wr32(s + 8, tcc.rcv_nxt);
    s[12] = (unsigned char)(5u << 4);
    s[13] = flags;
    wr16(s + 14, 4096u);
    wr16(s + 16, 0u);
    wr16(s + 18, 0u);
    for (i = 0; i < plen; i++)
        s[TCP_HDRLEN + i] = payload[i];
    wr16(s + 16, tcp_csum(NET_IP, tcc.rip, s, seglen));

    /* SEMENTARA Q2b: dump 8 byte pertama payload TCP */
    if (plen > 0) {
        unsigned di;
        for (di = 0; di < plen && di < 8u; di++)
            printf("%02x", payload[di]);
        printf("\r\n");
    }

    if (netstack_ip_send(tcc.rip, 6u, s, seglen) == 0) {
        st_tx++;
        tcc.snd_nxt += plen;
        if (flags & (TF_SYN | TF_FIN))
            tcc.snd_nxt++;
        tcc.last_seglen = seglen;
        tcc.last_flags = flags;
        tcc.last_tx_ms = tcc_now();
        /* Pure ACK tak perlu retransmit. */
        tcc.last_valid = (flags & (TF_SYN | TF_FIN)) || plen > 0;
    }
    /* Gagal (mis. ARP belum resolved): pemanggil/tick mencoba lagi. */
}

static void tcc_ack(void)
{
    unsigned char s[TCP_HDRLEN];

    wr16(s + 0, tcc.lport);
    wr16(s + 2, tcc.rport);
    wr32(s + 4, tcc.snd_nxt);
    wr32(s + 8, tcc.rcv_nxt);
    s[12] = (unsigned char)(5u << 4);
    s[13] = TF_ACK;
    wr16(s + 14, 4096u);
    wr16(s + 16, 0u);
    wr16(s + 18, 0u);
    wr16(s + 16, tcp_csum(NET_IP, tcc.rip, s, TCP_HDRLEN));
    if (netstack_ip_send(tcc.rip, 6u, s, TCP_HDRLEN) == 0)
        st_tx++;
}

/* Data masuk yang valid: tampung ke buffer RX (ack hanya yang tertampung). */
static void tcc_on_data(unsigned int seq, const unsigned char *t,
                        unsigned doff, unsigned datalen)
{
    unsigned i, room, take;

    if (datalen == 0)
        return;
    if (seq != tcc.rcv_nxt) {
        tcc_ack();      /* duplikat/out-of-order: ack ulang */
        return;
    }
    room = sizeof(tcc.rx) - tcc.rxlen;
    take = datalen < room ? datalen : room;
    for (i = 0; i < take; i++)
        tcc.rx[tcc.rxlen + i] = t[doff + i];
    tcc.rxlen += take;
    tcc.rcv_nxt += take;
    tcc_ack();
}

static void tcc_on_seg(unsigned int src, const unsigned char *eth_src,
                       unsigned short sport, unsigned int seq, unsigned int ack,
                       unsigned flags, const unsigned char *t,
                       unsigned doff, unsigned datalen)
{
    if (src != tcc.rip || sport != tcc.rport)
        return;
    if (flags & TF_RST) {
        tcc.state = TCC_CLOSED;
        return;
    }
    tcc.last_act = tcc_now();

    switch (tcc.state) {
    case TCC_SYN_SENT:
        if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK) &&
            ack == tcc.iss + 1u) {
            netstack_arp_learn(src, eth_src); /* MAC dari SYN-ACK */
            tcc.rcv_nxt = seq + 1u;
            tcc.snd_una = ack;
            tcc.last_valid = 0;
            tcc_ack();
            tcc.state = TCC_ESTABLISHED;
            if (datalen > 0)
                tcc_on_data(seq + 1u, t, doff, datalen);
        }
        break;

    case TCC_ESTABLISHED:
        if ((flags & TF_ACK) && ack > tcc.snd_una && ack <= tcc.snd_nxt) {
            tcc.snd_una = ack;
            if (tcc.snd_una == tcc.snd_nxt)
                tcc.last_valid = 0;
        }
        if (datalen > 0) {
            tcc_on_data(seq, t, doff, datalen);
        } else if (flags & TF_FIN) {
            tcc.rcv_nxt++;
            tcc_ack();
            tcc.peer_closed = 1;
            tcc.state = TCC_CLOSED;
        }
        break;

    case TCC_FIN_SENT:
        if (datalen > 0)
            tcc_on_data(seq, t, doff, datalen);
        if ((flags & TF_ACK) && ack >= tcc.snd_nxt) {
            tcc.state = TCC_CLOSED;
        } else if (flags & TF_FIN) {
            tcc.rcv_nxt++;
            tcc_ack();
        }
        break;

    default:
        break;
    }
}

/* --- API publik untuk syscall --- */

int tcp_client_connect(unsigned int dst_ip, unsigned short dst_port)
{
    if (tcc.state != TCC_CLOSED)
        return -1;              /* satu koneksi client dalam satu waktu */
    if (dst_ip == 0u || dst_port == 0u)
        return -1;
    tcc.rip = dst_ip;
    tcc.rport = dst_port;
    tcc.lport = tcc_next_port++;
    if (tcc_next_port < 0xD000u)
        tcc_next_port = 0xD000u;
    tcc.iss = tcc_now() ^ 0x5e3a7c1du;
    if (tcc.iss == 0u)
        tcc.iss = 1u;
    tcc.snd_nxt = tcc.iss;
    tcc.snd_una = tcc.iss;
    tcc.rcv_nxt = 0u;
    tcc.rxlen = 0u;
    tcc.peer_closed = 0;
    tcc.last_valid = 0;
    tcc.last_act = tcc_now();
    tcc.last_tx_ms = 0u;        /* tick langsung coba bila SYN pertama gagal */
    tcc.state = TCC_SYN_SENT;
    tcc_send(TF_SYN, 0, 0);     /* boleh gagal (ARP): tick kirim ulang */

    return 0;
}

/* 0=CLOSED 1=SYN_SENT 2=ESTABLISHED 3=FIN_SENT */
int tcp_client_status(void)
{
    return (int)tcc.state;
}

/* Kirim (stop-and-wait): n byte, 0 = coba lagi, -1 = tak tersambung. */
int tcp_client_send(const unsigned char *p, unsigned len)
{
    unsigned n;

    if (tcc.state != TCC_ESTABLISHED)
        return -1;
    if (tcc.last_valid) {
        return 0;               /* masih ada yang belum di-ack */
    }
    n = len < TCP_MAXSEG ? len : TCP_MAXSEG;
    tcc_send(TF_PSH | TF_ACK, p, n);
    return tcc.last_valid ? (int)n : 0;
}

/* Baca dari buffer RX: n byte, 0 = belum ada data. */
int tcp_client_recv(unsigned char *buf, unsigned maxlen)
{
    unsigned n, i;

    if (tcc.rxlen == 0)
        return 0;
    n = tcc.rxlen < maxlen ? tcc.rxlen : maxlen;
    for (i = 0; i < n; i++)
        buf[i] = tcc.rx[i];
    for (i = n; i < tcc.rxlen; i++)
        tcc.rx[i - n] = tcc.rx[i];
    tcc.rxlen -= n;
    return (int)n;
}

void tcp_client_close(void)
{
    if (tcc.state != TCC_ESTABLISHED)
        return;
    tcc_send(TF_FIN | TF_ACK, 0, 0);
    tcc.state = TCC_FIN_SENT;
}

/* Dipanggil dari tcp_tick(): retransmit SYN/data/FIN + timeout client. */
void tcp_client_tick(void)
{
    unsigned now = tcc_now();
    unsigned char *s;

    if (tcc.state == TCC_CLOSED)
        return;
    if (now - tcc.last_act > CONN_TO_MS) {
        tcc.state = TCC_CLOSED;
        return;
    }
    if (now - tcc.last_tx_ms <= RTO_MS)
        return;
    if (tcc.state == TCC_SYN_SENT) {
        /* SYN (atau ARP-nya) belum sampai: kirim ulang, seq tetap. */
        tcc.snd_nxt = tcc.iss;
        tcc_send(TF_SYN, 0, 0);
        return;
    }
    if (tcc.last_valid && tcc.snd_una < tcc.snd_nxt) {
        /* Data/FIN: kirim ulang salinan terakhir, patch ack number. */
        s = tcc.last_seg;
        s[8] = (unsigned char)(tcc.rcv_nxt >> 24);
        s[9] = (unsigned char)(tcc.rcv_nxt >> 16);
        s[10] = (unsigned char)(tcc.rcv_nxt >> 8);
        s[11] = (unsigned char)tcc.rcv_nxt;
        wr16(s + 16, 0u);
        wr16(s + 16, tcp_csum(NET_IP, tcc.rip, s, tcc.last_seglen));
        if (netstack_ip_send(tcc.rip, 6u, s, tcc.last_seglen) == 0) {
            st_tx++;
            tcc.last_tx_ms = now;
        }
    }
}

void tcp_on_ip(const unsigned char *ip, unsigned iplen,
               unsigned int src, const unsigned char *eth_src)
{
    unsigned ihl, doff, datalen;
    const unsigned char *t;
    unsigned short sport, dport, flags;
    unsigned int seq, ack;

    if (iplen < 40u)
        return;
    ihl = (ip[0] & 0x0fu) * 4u;
    if (ihl < 20u || iplen < ihl + TCP_HDRLEN)
        return;
    t = ip + ihl;
    doff = (t[12] >> 4) * 4u;
    if (doff < TCP_HDRLEN || iplen < ihl + doff)
        return;
    /* Verifikasi checksum TCP. */
    if (tcp_csum(rd32(ip + 12), rd32(ip + 16), t, iplen - ihl) != 0u)
        return;

    sport = rd16(t + 0);
    dport = rd16(t + 2);
    seq = (unsigned int)t[4] << 24 | (unsigned int)t[5] << 16 |
          (unsigned int)t[6] << 8 | t[7];
    ack = (unsigned int)t[8] << 24 | (unsigned int)t[9] << 16 |
          (unsigned int)t[10] << 8 | t[11];
    flags = t[13];
    datalen = iplen - ihl - doff;
    st_rx++;

    if (dport != TCP_PORT) {
        /* Q2a: segmen untuk TCP client? */
        if (tcc.state != TCC_CLOSED && dport == tcc.lport)
            tcc_on_seg(src, eth_src, sport, seq, ack, flags,
                       t, doff, datalen);
        return;                 /* bukan untuk kita */
    }

    if (flags & TF_RST) {
        tcp_reset_conn();
        return;
    }

    switch (tc.state) {
    case TS_LISTEN:
        if (flags & TF_SYN) {
            tc.rip = src;
            tc.rport = sport;
            tc.rcv_nxt = seq + 1u;
            tc.iss = (arm_timer_ticks() * 10u) ^ 0x1a2b3c4du;
            if (tc.iss == 0u)
                tc.iss = 1u;
            tc.snd_nxt = tc.iss;
            tc.snd_una = tc.iss;
            tc.reqlen = 0;
            tc.responded = 0;
            tc.last_act = (arm_timer_ticks() * 10u);
            netstack_arp_learn(src, eth_src); /* MAC dari frame SYN */
            tcp_send(TF_SYN | TF_ACK, 0, 0);
            tc.state = TS_SYN_RCVD;
            st_conns++;
            printf("[tcp] SYN -> SYN+ACK\r\n");
        }
        break;

    case TS_SYN_RCVD:
        if (src != tc.rip || sport != tc.rport)
            break;
        if (flags & TF_SYN) {
            /* SYN duplikat: kirim ulang SYN+ACK. */
            tc.snd_nxt = tc.iss; /* reset agar tcp_send hitung ulang */
            tcp_send(TF_SYN | TF_ACK, 0, 0);
            break;
        }
        if ((flags & TF_ACK) && ack == tc.iss + 1u) {
            tc.snd_una = ack;
            tc.last_valid = 0;  /* SYN+ACK sudah di-ack */
            tc.state = TS_ESTABLISHED;
            tc.last_act = (arm_timer_ticks() * 10u);
            printf("[tcp] ESTABLISHED\r\n");
            /* ACK bisa membawa data (digabung client). */
            tcp_on_data(seq, t, doff, datalen);
        }
        break;

    case TS_ESTABLISHED:
        if (src != tc.rip || sport != tc.rport)
            break;
        tc.last_act = (arm_timer_ticks() * 10u);
        if (flags & TF_SYN) {
            /* SYN baru di tengah koneksi: abaikan (klien nakal). */
            break;
        }
        /* Update snd_una dari ACK yang valid. */
        if ((flags & TF_ACK) && ack > tc.snd_una && ack <= tc.snd_nxt) {
            tc.snd_una = ack;
            if (tc.snd_una == tc.snd_nxt)
                tc.last_valid = 0;
        }
        if (datalen > 0) {
            tcp_on_data(seq, t, doff, datalen);
        } else if (flags & TF_FIN) {
            tc.rcv_nxt++;
            tcp_ack();
            if (!tc.responded)
                tcp_respond();  /* tetap layani lalu tutup */
            else {
                tcp_send(TF_FIN | TF_ACK, 0, 0);
                tc.state = TS_FIN_SENT;
            }
        } else if (flags & TF_ACK) {
            /* pure ACK: tak ada aksi */
        }
        break;

    case TS_FIN_SENT:
        if (src != tc.rip || sport != tc.rport)
            break;
        tc.last_act = (arm_timer_ticks() * 10u);
        if ((flags & TF_ACK) && ack >= tc.snd_nxt) {
            printf("[tcp] FIN di-ack -> LISTEN\r\n");
            tcp_reset_conn();
        } else if (flags & TF_FIN) {
            tc.rcv_nxt++;
            tcp_ack();
        }
        break;
    }
}

void tcp_tick(void)
{
    unsigned int now = (arm_timer_ticks() * 10u);

    tcp_client_tick();          /* Q2a: retransmit/timeout koneksi client */
    if (tc.state == TS_LISTEN)
        return;
    /* Koneksi buntu: reset paksa. */
    if (now - tc.last_act > CONN_TO_MS) {
        printf("[tcp] timeout koneksi -> LISTEN\r\n");
        tcp_reset_conn();
        return;
    }
    /* Retransmit: kirim ulang dari snd_una bila ada yang belum di-ack. */
    if (tc.last_valid && tc.snd_una < tc.snd_nxt &&
        now - tc.last_tx_ms > RTO_MS) {
        if (tc.tx_active && tc.snd_una >= tc.tx_seq0 &&
            tc.snd_una < tc.tx_seq0 + tc.tx_len) {
            /* Segmen data respons: bangun ulang dari buffer (tanpa
               menggeser snd_nxt dan tanpa menimpa salinan FIN). */
            static unsigned char rtx[TCP_HDRLEN + TCP_MAXSEG];
            unsigned off = (unsigned)(tc.snd_una - tc.tx_seq0);
            unsigned n = tc.tx_len - off;
            unsigned i;
            if (n > TCP_MAXSEG)
                n = TCP_MAXSEG;
            wr16(rtx + 0, TCP_PORT);
            wr16(rtx + 2, tc.rport);
            wr32(rtx + 4, tc.snd_una);
            wr32(rtx + 8, tc.rcv_nxt);
            rtx[12] = (unsigned char)(5u << 4);
            rtx[13] = (unsigned char)(TF_PSH | TF_ACK);
            wr16(rtx + 14, 4096u);
            wr16(rtx + 16, 0u);
            wr16(rtx + 18, 0u);
            for (i = 0; i < n; i++)
                rtx[TCP_HDRLEN + i] = tc.tx_base[off + i];
            wr16(rtx + 16, tcp_csum(NET_IP, tc.rip, rtx, TCP_HDRLEN + n));
            if (netstack_ip_send(tc.rip, 6u, rtx, TCP_HDRLEN + n) == 0) {
                st_tx++;
                tc.last_tx_ms = now;
            }
        } else {
            /* SYN+ACK / FIN murni: pakai salinan segmen terakhir. */
            unsigned char *s = tc.last_seg;
            /* Patch ack number (bisa maju sejak pengiriman). */
            s[8] = (unsigned char)(tc.rcv_nxt >> 24);
            s[9] = (unsigned char)(tc.rcv_nxt >> 16);
            s[10] = (unsigned char)(tc.rcv_nxt >> 8);
            s[11] = (unsigned char)tc.rcv_nxt;
            wr16(s + 16, 0u);
            wr16(s + 16, tcp_csum(NET_IP, tc.rip, s, tc.last_seglen));
            if (netstack_ip_send(tc.rip, 6u, s, tc.last_seglen) == 0) {
                st_tx++;
                tc.last_tx_ms = now;
            }
        }
    }
}

unsigned tcp_rx_segs(void) { return st_rx; }
unsigned tcp_tx_segs(void) { return st_tx; }
unsigned tcp_conns(void)   { return st_conns; }

/* Fase 12d: 1 bila tak ada koneksi aktif (state LISTEN). */
int tcp_is_listen(void) { return tc.state == TS_LISTEN; }
