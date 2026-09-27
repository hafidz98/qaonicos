/*
 * netstack.c - Ethernet + ARP + IPv4 + ICMP echo, Fase 11.
 *
 * Tanpa alokasi dinamis: buffer kirim statis, tabel ARP 8 entri.
 */
#include "netstack.h"
#include "net.h"


#define ETH_ALEN 6u
#define ETH_ARP  0x0806u
#define ETH_IP   0x0800u

#define ARP_REQ  1u
#define ARP_REP  2u

#define IP_ICMP  1u

#define ICMP_ECHO_REQ 8u
#define ICMP_ECHO_REP 0u

#define ARP_TABLE_N 8u

struct arp_ent {
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    uint8_t valid;
};

static struct arp_ent arp_tab[ARP_TABLE_N];
static uint8_t bcast_mac[ETH_ALEN] = {0xff,0xff,0xff,0xff,0xff,0xff};

static unsigned st_rx, st_arp_hit, st_ping_rep, st_ping_got;
static uint16_t ip_id;

/* --- Util --- */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
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
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    unsigned i;
    for (i = 0; i < ETH_ALEN; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static uint16_t csum(const uint8_t *p, unsigned len)
{
    uint32_t s = 0;
    unsigned i;
    for (i = 0; i + 1 < len; i += 2)
        s += (uint16_t)((p[i] << 8) | p[i + 1]);
    if (len & 1u)
        s += (uint16_t)(p[len - 1] << 8);
    while (s >> 16)
        s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)~s;
}

/* --- ARP --- */
static struct arp_ent *arp_find(uint32_t ip)
{
    unsigned i;
    for (i = 0; i < ARP_TABLE_N; i++)
        if (arp_tab[i].valid && arp_tab[i].ip == ip)
            return &arp_tab[i];
    return 0;
}
static void arp_learn(uint32_t ip, const uint8_t *mac)
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

static uint8_t txf[2048];

static void eth_send(const uint8_t *dst, uint16_t etype,
                     const uint8_t *payload, unsigned plen)
{
    unsigned i;
    const uint8_t *smac = net_mac();
    for (i = 0; i < ETH_ALEN; i++) {
        txf[i] = dst[i];
        txf[6 + i] = smac[i];
    }
    wr16(txf + 12, etype);
    for (i = 0; i < plen; i++)
        txf[14 + i] = payload[i];
    net_send(txf, 14u + plen);
}

static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip)
{
    uint8_t a[28];
    const uint8_t *smac = net_mac();
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

static void arp_on_frame(const uint8_t *f, unsigned len)
{
    uint16_t op;
    uint32_t spa, tpa;

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
        net_log("[arp] reply -> ");
        net_loghex(spa >> 24); net_log(".");
        net_loghex((spa >> 16) & 0xff); net_log(".");
        net_loghex((spa >> 8) & 0xff); net_log(".");
        net_loghex(spa & 0xff); net_log("\r\n");
    }
}

/* --- ICMP --- */
static void icmp_on_ip(const uint8_t *ip, unsigned iplen,
                       uint32_t src, uint32_t dst)
{
    unsigned ihl, icmp_len, i;
    uint8_t *o;

    (void)dst;
    if (iplen < 28u)
        return;
    ihl = (ip[0] & 0x0fu) * 4u;
    if (ihl < 20u || iplen < ihl + 8u)
        return;
    if (ip[9] != IP_ICMP)
        return;

    {
        const uint8_t *ic = ip + ihl;
        icmp_len = iplen - ihl;
        /* Verifikasi checksum. */
        if (csum(ic, icmp_len) != 0u)
            return;
        if (ic[0] == ICMP_ECHO_REP) {
            /* Balasan atas ping kita. */
            st_ping_got++;
            net_log("[icmp] echo reply diterima!\n");
            return;
        }
        if (ic[0] != ICMP_ECHO_REQ)
            return;

        /* Bangun echo reply in-place di txf. */
        o = txf + 14;
        o[0] = 0x45u; o[1] = 0u;
        wr16(o + 2, (uint16_t)(20u + icmp_len));
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
                net_log("[icmp] echo reply\r\n");
            }
        }
    }
}

static void ip_on_frame(const uint8_t *f, unsigned len)
{
    const uint8_t *ip = f + 14;
    unsigned iplen, ihl;
    uint32_t dst;

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
    if (dst != NET_IP)
        return;
    icmp_on_ip(ip, iplen, rd32(ip + 12), dst);
}

static void rx_handler(const uint8_t *f, unsigned len)
{
    uint16_t etype;

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
    /* Sejauh ini tak ada state periodik; placeholder untuk retry ARP. */
}

int netstack_ping(uint32_t ip)
{
    struct arp_ent *e = arp_find(ip);
    uint8_t *o;
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
        o[i] = (uint8_t)i;
    wr16(o + 22, csum(o + 20, 28u));

    eth_send(e->mac, ETH_IP, o, 48u);
    return 0;
}

unsigned netstack_rx_frames(void) { return st_rx; }
unsigned netstack_arp_hits(void)  { return st_arp_hit; }
unsigned netstack_ping_replies(void) { return st_ping_rep; }
unsigned netstack_ping_got(void)  { return st_ping_got; }
