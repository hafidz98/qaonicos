/*
 * tcp.c - Server TCP minimal (satu koneksi, port 80) untuk HTTP, Fase 12.
 *
 * Batasan yang disengaja: satu koneksi dalam satu waktu, tanpa window
 * scaling/SACK/timestamps, MSS implisit 536 (kita tak kirim opsi).
 * Data request diakumulasi sampai "\r\n\r\n" lalu http_handle() dipanggil
 * dan respons dikirim sekaligus (HTTP/1.0, Connection: close).
 */
#include "tcp.h"
#include "net.h"
#include "netstack.h"
#include "sched.h"
#include "http.h"

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

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xffu);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* Checksum TCP: pseudo-header + segmen. */
static uint16_t tcp_csum(uint32_t src, uint32_t dst,
                         const uint8_t *seg, unsigned seglen)
{
    uint32_t s = 0;
    unsigned i;

    s += (src >> 16) & 0xffffu;
    s += src & 0xffffu;
    s += (dst >> 16) & 0xffffu;
    s += dst & 0xffffu;
    s += 6u;                 /* protocol = TCP */
    s += seglen;
    for (i = 0; i + 1 < seglen; i += 2)
        s += (uint16_t)((seg[i] << 8) | seg[i + 1]);
    if (seglen & 1u)
        s += (uint16_t)(seg[seglen - 1] << 8);
    while (s >> 16)
        s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)~s;
}

static struct {
    uint8_t  state;
    uint32_t rip;                 /* IP remote */
    uint16_t rport;               /* port remote */
    uint32_t iss;                 /* initial seq kita */
    uint32_t snd_nxt;             /* seq berikutnya untuk kirim */
    uint32_t snd_una;             /* seq tertua yang belum di-ack */
    uint32_t rcv_nxt;             /* seq berikutnya yang diharapkan */
    uint32_t last_act;            /* sched_ticks() terakhir ada aktivitas */
    /* Segmen terakhir yang butuh ack (untuk retransmit): */
    uint8_t  last_seg[TCP_HDRLEN + TCP_MAXSEG];
    unsigned last_seglen;         /* panjang segmen TCP (header+payload) */
    uint8_t  last_flags;          /* flags segmen terakhir */
    uint32_t last_tx_ms;          /* kapan terakhir dikirim */
    uint8_t  last_valid;
    /* Buffer request HTTP: */
    uint8_t  req[1024];
    unsigned reqlen;
    uint8_t  responded;
} tc;

static unsigned st_rx, st_tx, st_conns;

/* Kirim segmen TCP. flags = kombinasi TF_*. payload boleh NULL/0.
 * Meng-update snd_nxt & menyimpan salinan untuk retransmit bila segmen
 * membawa SYN/FIN/data (pure ACK tak perlu retransmit). */
static void tcp_send(uint8_t flags, const uint8_t *payload, unsigned plen)
{
    uint8_t *s = tc.last_seg; /* bangun langsung di buffer retransmit */
    unsigned seglen = TCP_HDRLEN + plen;

    wr16(s + 0, TCP_PORT);
    wr16(s + 2, tc.rport);
    wr32(s + 4, tc.snd_nxt);
    wr32(s + 8, tc.rcv_nxt);
    s[12] = (uint8_t)(5u << 4);  /* data offset = 5 (20 byte), tanpa opsi */
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
        tc.last_tx_ms = sched_ticks();
        /* Pure ACK tak perlu di-retransmit. */
        tc.last_valid = (flags & (TF_SYN | TF_FIN)) || plen > 0;
    }
}

/* ACK murni (tak mengubah snd_nxt, tak disimpan untuk retransmit). */
static void tcp_ack(void)
{
    uint8_t s[TCP_HDRLEN];

    wr16(s + 0, TCP_PORT);
    wr16(s + 2, tc.rport);
    wr32(s + 4, tc.snd_nxt);
    wr32(s + 8, tc.rcv_nxt);
    s[12] = (uint8_t)(5u << 4);
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
}

/* Kirim respons HTTP lalu FIN (dipanggil sekali saat request lengkap). */
static void tcp_respond(void)
{
    static uint8_t resp[1400];
    unsigned rlen;

    tc.responded = 1;
    rlen = http_handle(tc.req, tc.reqlen, resp);
    if (rlen > TCP_MAXSEG)
        rlen = TCP_MAXSEG;      /* pengaman; respons kita < 1 segmen */
    /* Kirim body; kalau > TCP_MAXSEG perlu segmentasi (tak terjadi). */
    tcp_send(TF_PSH | TF_ACK, resp, rlen);
    /* Langsung FIN (HTTP/1.0 Connection: close). */
    tcp_send(TF_FIN | TF_ACK, 0, 0);
    tc.state = TS_FIN_SENT;
    net_log("[tcp] respons dikirim, FIN\r\n");
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
static void tcp_on_data(uint32_t seq, const uint8_t *t,
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

void tcp_on_ip(const uint8_t *ip, unsigned iplen,
               uint32_t src, const uint8_t *eth_src)
{
    unsigned ihl, doff, datalen;
    const uint8_t *t;
    uint16_t sport, dport, flags;
    uint32_t seq, ack;

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
    seq = (uint32_t)t[4] << 24 | (uint32_t)t[5] << 16 |
          (uint32_t)t[6] << 8 | t[7];
    ack = (uint32_t)t[8] << 24 | (uint32_t)t[9] << 16 |
          (uint32_t)t[10] << 8 | t[11];
    flags = t[13];
    datalen = iplen - ihl - doff;
    st_rx++;

    if (dport != TCP_PORT)
        return;                 /* bukan untuk kita */

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
            tc.iss = sched_ticks() ^ 0x1a2b3c4du;
            if (tc.iss == 0u)
                tc.iss = 1u;
            tc.snd_nxt = tc.iss;
            tc.snd_una = tc.iss;
            tc.reqlen = 0;
            tc.responded = 0;
            tc.last_act = sched_ticks();
            netstack_arp_learn(src, eth_src); /* MAC dari frame SYN */
            tcp_send(TF_SYN | TF_ACK, 0, 0);
            tc.state = TS_SYN_RCVD;
            st_conns++;
            net_log("[tcp] SYN -> SYN+ACK\r\n");
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
            tc.last_act = sched_ticks();
            net_log("[tcp] ESTABLISHED\r\n");
            /* ACK bisa membawa data (digabung client). */
            tcp_on_data(seq, t, doff, datalen);
        }
        break;

    case TS_ESTABLISHED:
        if (src != tc.rip || sport != tc.rport)
            break;
        tc.last_act = sched_ticks();
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
        tc.last_act = sched_ticks();
        if ((flags & TF_ACK) && ack >= tc.snd_nxt) {
            net_log("[tcp] FIN di-ack -> LISTEN\r\n");
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
    uint32_t now = sched_ticks();

    if (tc.state == TS_LISTEN)
        return;
    /* Koneksi buntu: reset paksa. */
    if (now - tc.last_act > CONN_TO_MS) {
        net_log("[tcp] timeout koneksi -> LISTEN\r\n");
        tcp_reset_conn();
        return;
    }
    /* Retransmit segmen terakhir yang belum di-ack. */
    if (tc.last_valid && now - tc.last_tx_ms > RTO_MS) {
        uint8_t *s = tc.last_seg;
        /* Patch ack number (bisa maju sejak pengiriman). */
        s[8] = (uint8_t)(tc.rcv_nxt >> 24);
        s[9] = (uint8_t)(tc.rcv_nxt >> 16);
        s[10] = (uint8_t)(tc.rcv_nxt >> 8);
        s[11] = (uint8_t)tc.rcv_nxt;
        wr16(s + 16, 0u);
        wr16(s + 16, tcp_csum(NET_IP, tc.rip, s, tc.last_seglen));
        if (netstack_ip_send(tc.rip, 6u, s, tc.last_seglen) == 0) {
            st_tx++;
            tc.last_tx_ms = now;
        }
    }
}

unsigned tcp_rx_segs(void) { return st_rx; }
unsigned tcp_tx_segs(void) { return st_tx; }
unsigned tcp_conns(void)   { return st_conns; }
