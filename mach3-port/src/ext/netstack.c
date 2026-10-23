/*
 * mach3/kernel/arm/netstack.c -- Ethernet + ARP + IPv4 + ICMP echo
 * (Fase D).
 *
 * Port dari archive/kernel-scratch/kernel/src/netstack.c (Fase 11).
 * Tanpa alokasi dinamis: buffer kirim statis, tabel ARP 8 entri.
 */

/* Driver virtio-net (net.c). */
extern const unsigned char *net_mac(void);
extern int	net_send(const unsigned char *frame, unsigned len);
extern void	net_on_rx(void (*cb)(const unsigned char *, unsigned));

/* TCP (tcp.c): dipanggil untuk paket IP TCP. */
extern void	tcp_on_ip(const unsigned char *ip, unsigned iplen,
			    unsigned int src, const unsigned char *smac);

/* Timer (clock.c): ticks 100 Hz. */
extern unsigned int	arm_timer_ticks(void);
extern int		printf(const char *, ...);

/* IP guest: 10.0.2.15 (cocok default QEMU user-net). */
#define	NET_IP	0x0A00020Fu

#define ETH_ALEN 6u
#define ETH_ARP  0x0806u
#define ETH_IP   0x0800u

#define ARP_REQ  1u
#define ARP_REP  2u

#define IP_ICMP  1u
#define IP_TCP   6u
#define IP_UDP   17u

#define ICMP_ECHO_REQ 8u
#define ICMP_ECHO_REP 0u

#define ARP_TABLE_N 8u

struct arp_ent {
    unsigned int ip;
    unsigned char mac[ETH_ALEN];
    unsigned char valid;
};

static struct arp_ent arp_tab[ARP_TABLE_N];
static unsigned char bcast_mac[ETH_ALEN] = {0xff,0xff,0xff,0xff,0xff,0xff};

static unsigned st_rx, st_arp_hit, st_ping_rep, st_ping_got;
static unsigned short ip_id;

/* --- Util --- */
static unsigned short rd16(const unsigned char *p)
{
    return (unsigned short)((p[0] << 8) | p[1]);
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
static unsigned int rd32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | p[3];
}
static int mac_eq(const unsigned char *a, const unsigned char *b)
{
    unsigned i;
    for (i = 0; i < ETH_ALEN; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static unsigned short csum(const unsigned char *p, unsigned len)
{
    unsigned int s = 0;
    unsigned i;
    for (i = 0; i + 1 < len; i += 2)
        s += (unsigned short)((p[i] << 8) | p[i + 1]);
    if (len & 1u)
        s += (unsigned short)(p[len - 1] << 8);
    while (s >> 16)
        s = (s & 0xffffu) + (s >> 16);
    return (unsigned short)~s;
}

/* --- ARP --- */
static struct arp_ent *arp_find(unsigned int ip)
{
    unsigned i;
    for (i = 0; i < ARP_TABLE_N; i++)
        if (arp_tab[i].valid && arp_tab[i].ip == ip)
            return &arp_tab[i];
    return 0;
}
static void arp_learn(unsigned int ip, const unsigned char *mac)
{
    struct arp_ent *e = arp_find(ip);
    unsigned i;
    if (!e) {
        e = &arp_tab[0];
        for (i = 0; i < ARP_TABLE_N; i++)
            if (!arp_tab[i].valid) { e = &arp_tab[i]; break; }
    }
    e->ip = ip;
    for (i = 0; i < ETH_ALEN; i++)
        e->mac[i] = mac[i];
    e->valid = 1u;
}

static unsigned char txf[2048];

static void eth_send(const unsigned char *dst, unsigned short etype,
                     const unsigned char *payload, unsigned plen)
{
    unsigned i;
    const unsigned char *smac = net_mac();
    for (i = 0; i < ETH_ALEN; i++) {
        txf[i] = dst[i];
        txf[6 + i] = smac[i];
    }
    wr16(txf + 12, etype);
    for (i = 0; i < plen; i++)
        txf[14 + i] = payload[i];
    net_send(txf, 14u + plen);
}

static void arp_send(unsigned short op, const unsigned char *tmac, unsigned int tip)
{
    unsigned char a[28];
    const unsigned char *smac = net_mac();
    unsigned i;

    wr16(a + 0, 1u);          /* HTYPE Ethernet */
    wr16(a + 2, ETH_IP);      /* PTYPE IPv4 */
    a[4] = 6u; a[5] = 4u;
    wr16(a + 6, op);
    for (i = 0; i < ETH_ALEN; i++) {
        a[8 + i] = smac[i];
        a[18 + i] = tmac[i];
    }
    wr32(a + 14, NET_IP);
    wr32(a + 24, tip);
    eth_send(tmac, ETH_ARP, a, 28u);
}

static void arp_on_frame(const unsigned char *f, unsigned len)
{
    unsigned short op;
    unsigned int spa, tpa;

    if (len < 14u + 28u)
        return;
    if (rd16(f + 14) != 1u || rd16(f + 16) != ETH_IP)
        return;
    if (f[18] != 6u || f[19] != 4u)
        return;
    op = rd16(f + 20);
    spa = rd32(f + 28);
    tpa = rd32(f + 38);

    arp_learn(spa, f + 22);

    if (op == ARP_REQ && tpa == NET_IP) {
        /* Reply: target = pengirim. */
        arp_send(ARP_REP, f + 22, spa);
        st_arp_hit++;
        printf("[arp] reply -> ");
        printf("%02x", (unsigned)(spa >> 24)); printf(".");
        printf("%02x", (unsigned)((spa >> 16) & 0xff)); printf(".");
        printf("%02x", (unsigned)((spa >> 8) & 0xff)); printf(".");
        printf("%02x", (unsigned)(spa & 0xff)); printf("\r\n");
    }
}

/* --- UDP (App A4: untuk DNS + NTP) ---
 *
 * Model minimal: slot RX tunggal 512 byte.  udp_expect_port dicatat
 * setiap udp_send; paket masuk yang dport-nya cocok disalin ke slot.
 * Cukup untuk pola request->response sekuensial (DNS lalu NTP).
 */
#define UDP_RX_MAX 512u


/* Didefinisikan di bawah (Fase 12); forward decl untuk udp_send. */
int	netstack_ip_send(unsigned int dst, unsigned char proto,
			  const unsigned char *payload, unsigned plen);

static unsigned char udp_rx_data[UDP_RX_MAX];
static unsigned udp_rx_len;
static unsigned int udp_rx_src;
static unsigned short udp_rx_sport;
static unsigned char udp_rx_valid;
static unsigned short udp_expect_port;
static unsigned short udp_ephem = 0xC000u;

static void
udp_on_ip(const unsigned char *ip, unsigned iplen, unsigned int src)
{
    unsigned ihl, ulen, dlen, sport, dport, i;
    const unsigned char *u;

    ihl = (ip[0] & 0x0fu) * 4u;
    if (ihl < 20u || iplen < ihl + 8u)
        return;
    u = ip + ihl;
    sport = rd16(u + 0);
    dport = rd16(u + 2);
    ulen = rd16(u + 4);
    if (ulen < 8u || iplen < ihl + ulen)
        return;
    if (dport != udp_expect_port)
        return;                 /* bukan balasan yang ditunggu */
    /* UDP checksum opsional (0 = tidak ada); terima apa adanya. */
    dlen = ulen - 8u;
    if (dlen > UDP_RX_MAX)
        dlen = UDP_RX_MAX;
    for (i = 0; i < dlen; i++)
        udp_rx_data[i] = u[8 + i];
    udp_rx_len = dlen;
    udp_rx_src = src;
    udp_rx_sport = (unsigned short)sport;
    udp_rx_valid = 1u;
}

/* Kirim datagram UDP.  Mengembalikan source port yang dipakai, atau
 * -1 bila MAC tujuan belum dikenal (pemanggil kirim ARP dulu / coba
 * lagi) atau paket kebesaran. */
int
netstack_udp_send(unsigned int dst, unsigned short dport,
                  const unsigned char *data, unsigned dlen)
{
    static unsigned char upkt[8 + UDP_RX_MAX];
    unsigned short sport;
    unsigned i;

    if (dlen > UDP_RX_MAX)
        return -1;
    if (arp_find(dst) == 0) {
        arp_send(ARP_REQ, bcast_mac, dst);
        return -1;
    }
    sport = udp_ephem++;
    if (udp_ephem < 0xC000u)
        udp_ephem = 0xC000u;
    wr16(upkt + 0, sport);
    wr16(upkt + 2, dport);
    wr16(upkt + 4, (unsigned short)(8u + dlen));
    wr16(upkt + 6, 0u);         /* checksum 0 = tidak dipakai */
    for (i = 0; i < dlen; i++)
        upkt[8 + i] = data[i];
    if (netstack_ip_send(dst, IP_UDP, upkt, 8u + dlen) != 0)
        return -1;
    udp_expect_port = sport;
    udp_rx_valid = 0u;          /* buang balasan basi */
    return (int)sport;
}

/* Ambil datagram yang cocok (non-blocking).  >0 = jumlah byte,
 * 0 = belum ada, -1 = argumen buruk. */
int
netstack_udp_recv(unsigned char *buf, unsigned maxlen,
                  unsigned int *src_ip, unsigned short *src_port)
{
    unsigned i, n;

    if (!buf || maxlen == 0u)
        return -1;
    if (!udp_rx_valid)
        return 0;
    n = udp_rx_len < maxlen ? udp_rx_len : maxlen;
    for (i = 0; i < n; i++)
        buf[i] = udp_rx_data[i];
    if (src_ip)
        *src_ip = udp_rx_src;
    if (src_port)
        *src_port = udp_rx_sport;
    udp_rx_valid = 0u;
    return (int)n;
}

/* --- ICMP --- */
static void icmp_on_ip(const unsigned char *ip, unsigned iplen,
                       unsigned int src, unsigned int dst)
{
    unsigned ihl, icmp_len, i;
    unsigned char *o;

    (void)dst;
    if (iplen < 28u)
        return;
    ihl = (ip[0] & 0x0fu) * 4u;
    if (ihl < 20u || iplen < ihl + 8u)
        return;
    if (ip[9] != IP_ICMP)
        return;

    {
        const unsigned char *ic = ip + ihl;
        icmp_len = iplen - ihl;
        /* Verifikasi checksum. */
        if (csum(ic, icmp_len) != 0u)
            return;
        if (ic[0] == ICMP_ECHO_REP) {
            /* Balasan atas ping kita. */
            st_ping_got++;
            printf("[icmp] echo reply diterima!\n");
            return;
        }
        if (ic[0] != ICMP_ECHO_REQ)
            return;

        /* Bangun echo reply in-place di txf. */
        o = txf + 14;
        o[0] = 0x45u; o[1] = 0u;
        wr16(o + 2, (unsigned short)(20u + icmp_len));
        wr16(o + 4, ip_id++);
        o[6] = 0u; o[7] = 0u;
        o[8] = 64u; o[9] = IP_ICMP;
        wr16(o + 10, 0u);
        wr32(o + 12, NET_IP);
        wr32(o + 16, src);
        wr16(o + 10, csum(o, 20u));

        for (i = 0; i < icmp_len; i++)
            o[20 + i] = ic[i];
        o[20] = ICMP_ECHO_REP;
        o[21] = 0u;
        wr16(o + 22, 0u);
        wr16(o + 22, csum(o + 20, icmp_len));

        {
            struct arp_ent *e = arp_find(src);
            if (e) {
                eth_send(e->mac, ETH_IP, o, 20u + icmp_len);
                st_ping_rep++;
                printf("[icmp] echo reply\r\n");
            }
        }
    }
}

static void ip_on_frame(const unsigned char *f, unsigned len)
{
    const unsigned char *ip = f + 14;
    unsigned iplen, ihl;
    unsigned int dst, src;

    if (len < 14u + 20u)
        return;
    if ((ip[0] >> 4) != 4u)
        return;
    ihl = (ip[0] & 0x0fu) * 4u;
    if (ihl < 20u)
        return;
    iplen = rd16(ip + 2);
    if (iplen > len - 14u || iplen < ihl)
        return;
    if (csum(ip, ihl) != 0u)
        return;
    dst = rd32(ip + 16);
    src = rd32(ip + 12);
    if (dst != NET_IP)
        return;
    if (ip[9] == IP_ICMP)
        icmp_on_ip(ip, iplen, src, dst);
    else if (ip[9] == IP_TCP)
        tcp_on_ip(ip, iplen, src, f + 6); /* f+6 = MAC sumber */
    else if (ip[9] == IP_UDP)
        udp_on_ip(ip, iplen, src);
}

/* Fase 12: kirim paket IP generik (dipakai TCP; dideklarasikan di atas
 * untuk UDP App A4). */
int netstack_ip_send(unsigned int dst, unsigned char proto,
                     const unsigned char *payload, unsigned plen)
{
    struct arp_ent *e = arp_find(dst);
    unsigned char *o;
    unsigned i;

    if (!e)
        return -1;
    if (plen > 1400u)
        return -1;
    o = txf + 14;
    o[0] = 0x45u; o[1] = 0u;
    wr16(o + 2, (unsigned short)(20u + plen));
    wr16(o + 4, ip_id++);
    o[6] = 0u; o[7] = 0u;
    o[8] = 64u; o[9] = proto;
    wr16(o + 10, 0u);
    wr32(o + 12, NET_IP);
    wr32(o + 16, dst);
    wr16(o + 10, csum(o, 20u));
    for (i = 0; i < plen; i++)
        o[20 + i] = payload[i];
    eth_send(e->mac, ETH_IP, o, 20u + plen);
    return 0;
}

/* Fase 12: belajar MAC dari frame (dipakai TCP saat SYN). */
void netstack_arp_learn(unsigned int ip, const unsigned char *mac)
{
    arp_learn(ip, mac);
}

static void rx_handler(const unsigned char *f, unsigned len)
{
    unsigned short etype;

    st_rx++;
    if (len < 14u)
        return;
    /* Terima jika dst = MAC kita atau broadcast. */
    if (!mac_eq(f, net_mac()) && !mac_eq(f, bcast_mac))
        return;
    etype = rd16(f + 12);
    if (etype == ETH_ARP)
        arp_on_frame(f, len);
    else if (etype == ETH_IP)
        ip_on_frame(f, len);
}

void netstack_init(void)
{
    unsigned i;
    for (i = 0; i < ARP_TABLE_N; i++)
        arp_tab[i].valid = 0u;
    net_on_rx(rx_handler);
}

void netstack_tick(void)
{
    tcp_tick();   /* retransmit TCP + timeout koneksi */
}

int netstack_ping(unsigned int ip)
{
    struct arp_ent *e = arp_find(ip);
    unsigned char *o;
    unsigned i;

    if (!e) {
        /* Minta MAC dulu; pemanggil bisa coba lagi. */
        arp_send(ARP_REQ, bcast_mac, ip);
        return -1;
    }
    o = txf + 14;
    o[0] = 0x45u; o[1] = 0u;
    wr16(o + 2, 48u);
    wr16(o + 4, ip_id++);
    o[6] = 0u; o[7] = 0u;
    o[8] = 64u; o[9] = IP_ICMP;
    wr16(o + 10, 0u);
    wr32(o + 12, NET_IP);
    wr32(o + 16, ip);
    wr16(o + 10, csum(o, 20u));

    o[20] = ICMP_ECHO_REQ; o[21] = 0u;
    wr16(o + 22, 0u);
    wr16(o + 24, 0x1234u);   /* identifier */
    wr16(o + 26, 0x0001u);   /* sequence */
    for (i = 28; i < 48; i++)
        o[i] = (unsigned char)i;
    wr16(o + 22, csum(o + 20, 28u));

    eth_send(e->mac, ETH_IP, o, 48u);
    return 0;
}

unsigned netstack_rx_frames(void) { return st_rx; }
unsigned netstack_arp_hits(void)  { return st_arp_hit; }
unsigned netstack_ping_replies(void) { return st_ping_rep; }
unsigned netstack_ping_got(void)  { return st_ping_got; }
