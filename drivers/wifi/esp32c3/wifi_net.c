/* Minimal IPv4 stack for the ESP32-C3 WiFi station.
 *
 * TinyOS has no TCP/IP stack and the remaining XIP window is too small for
 * lwIP, so this file implements just what a station needs to be reachable:
 * ARP, IPv4, ICMP echo (both directions) and a DHCP client.  Data frames
 * arrive through the driver's Ethernet RX callback and go out through
 * esp_wifi_internal_tx().
 *
 * /dev/net exposes the state and a tiny command interface:
 *   read   -> "ip ... gw ... dns ... state ..."
 *   write  -> "ping <a.b.c.d> [count]" or "dhcp"
 */
#include "types.h"
#include "string.h"
#include "console.h"
#include "platform.h"
#include "process.h"
#include "devfs.h"

#ifdef TOS_NET

extern int esp_wifi_internal_reg_rxcb(int ifx, int (*fn)(void *, uint16_t, void *));
extern void esp_wifi_internal_free_rx_buffer(void *buffer);
extern int esp_wifi_internal_tx(int ifx, void *buf, uint16_t len);
extern int esp_wifi_get_macaddr_internal(uint8_t ifx, uint8_t *mac);

#define ETH_ALEN 6
#define ETH_HDR  14
#define MTU      1500

#define ETH_ARP  0x0806
#define ETH_IP   0x0800
#define IP_ICMP  1
#define IP_TCP   6
#define IP_UDP   17

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DNS_PORT         53
#define DNS_LOCAL_PORT   5353

struct eth_hdr {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;              /* big endian */
} __attribute__((packed));

struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip_hdr {
    uint8_t vihl;
    uint8_t tos;
    uint16_t total;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t check;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t check;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t check;
} __attribute__((packed));

struct tcp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t off;
    uint8_t flags;
    uint16_t win;
    uint16_t check;
    uint16_t urg;
} __attribute__((packed));

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_CLOSED   0
#define TCP_SYN_SENT 1
#define TCP_ESTAB    2
#define TCP_DONE     3

static struct {
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
    uint32_t lease;
    int bound;
    int started;
    int dhcp_state;             /* 0 idle, 1 discover, 2 request, 3 bound */
    uint32_t dhcp_xid;
    uint32_t dhcp_next;
    int dhcp_tries;
    uint32_t dhcp_renew_at;     /* uptime ms for T1 */
    uint32_t dhcp_rebind_at;    /* uptime ms for T2 */
    int dhcp_renewing;          /* 0 none, 1 unicast renew, 2 broadcast rebind */
    uint32_t dhcp_t1_s, dhcp_t2_s;
    uint8_t dhcp_server_mac[6];
    int dhcp_server_mac_valid;
    volatile int dhcp_rx_type;  /* 2 offer, 5 ack, 6 nak */
    volatile uint32_t dhcp_yiaddr, dhcp_server, dhcp_mask, dhcp_gw, dhcp_dns;
    volatile uint32_t arp_want_ip;
    volatile int arp_got;
    volatile uint8_t arp_mac[6];
    volatile uint16_t ping_id;
    volatile uint16_t ping_seq;
    volatile uint16_t ping_reply_seq;
    volatile uint32_t ping_sent_ms;
    volatile uint32_t ping_rtt;
    volatile int ping_pending;
    volatile uint32_t ping_target;
    volatile int ping_count;
    uint8_t dns_mac[6];
    int dns_mac_valid;
    uint16_t dns_next_id;
    volatile uint16_t dns_id;
    volatile int dns_done, dns_ok;
    volatile uint32_t dns_result;
    char lookup_name[64];
    volatile int lookup_pending;
    char ping_host[64];
    volatile int ping_host_pending;
    uint8_t tcp_mac[6];
    int tcp_mac_valid;
    uint32_t tcp_remote_ip;
    uint16_t tcp_remote_port, tcp_local_port;
    uint32_t tcp_snd_nxt, tcp_snd_una, tcp_rcv_nxt, tcp_iss;
    volatile int tcp_state;
    volatile int tcp_got_synack, tcp_got_ack, tcp_got_data;
    volatile int tcp_got_fin, tcp_got_rst, tcp_rx_overflow;
    volatile int tcp_rx_len;
    uint8_t tcp_rx_buf[4096];
    uint8_t http_req[256];
    uint32_t http_req_seq;
    int http_req_len, http_req_retx;
    uint32_t http_req_next;
    char http_host[64];
    char http_path[64];
    uint32_t http_ip;
    int http_host_is_ip;
    uint16_t http_port;
    volatile int http_pending;
    uint32_t rx_frames, rx_ip, rx_udp, rx_icmp, rx_tcp, rx_drop;
} ns;

static uint8_t tx_frame[ETH_HDR + 20 + MTU];
static uint8_t rx_frame[ETH_HDR + 20 + MTU];
static uint8_t rx_payload[MTU];
static uint8_t dhcp_pkt[320];
static uint32_t dhcp_xid_next = 0x5a5a0001;

static uint16_t rd16(const void *p)
{
    const uint8_t *b = p;

    return (uint16_t)((b[0] << 8) | b[1]);
}

static uint32_t rd32(const void *p)
{
    const uint8_t *b = p;

    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static void wr16(void *p, uint16_t v)
{
    uint8_t *b = p;

    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static void wr32(void *p, uint32_t v)
{
    uint8_t *b = p;

    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static uint32_t csum_add(uint32_t sum, const uint8_t *p, int len)
{
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t csum_fin(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t ip_checksum(const uint8_t *p, int len)
{
    return csum_fin(csum_add(0, p, len));
}

static int ip_is_ours(uint32_t ip)
{
    return ip == ns.ip || ip == 0xffffffffu;
}

/* ---- transmit -------------------------------------------------------- */

static int send_eth(uint8_t *frame, const uint8_t *dst, uint16_t type,
                    const uint8_t *payload, int len)
{
    struct eth_hdr *e = (struct eth_hdr *)frame;

    if (len < 0 || len > MTU)
        return -1;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, ns.mac, 6);
    wr16(&e->type, type);
    if (payload)
        memcpy(frame + ETH_HDR, payload, (size_t)len);
    return esp_wifi_internal_tx(0, frame, (uint16_t)(ETH_HDR + len));
}

/* Build IP in place then hand the whole frame to the driver. */
static int transmit_ip(const uint8_t *dst_mac, uint32_t dst_ip, uint8_t proto,
                       const uint8_t *payload, int len, int from_rx)
{
    uint8_t *frame = from_rx ? rx_frame : tx_frame;
    uint32_t src = ns.ip;

    if (!ns.bound && proto != IP_UDP)
        return -1;
    if (len < 0 || len > MTU - 20)
        return -1;
    {
        struct eth_hdr *e = (struct eth_hdr *)frame;
        struct ip_hdr *ip = (struct ip_hdr *)(frame + ETH_HDR);

        memcpy(e->dst, dst_mac, 6);
        memcpy(e->src, ns.mac, 6);
        wr16(&e->type, ETH_IP);
        memset(ip, 0, sizeof(*ip));
        ip->vihl = 0x45;
        wr16(&ip->total, (uint16_t)(20 + len));
        wr16(&ip->id, (uint16_t)(plat_uptime_ms() & 0xffff));
        ip->ttl = 64;
        ip->proto = proto;
        wr32((uint8_t *)ip + 12, src);
        wr32((uint8_t *)ip + 16, dst_ip);
        wr16(&ip->check, 0);
        wr16(&ip->check, ip_checksum((const uint8_t *)ip, 20));
        if (len)
            memcpy(frame + ETH_HDR + 20, payload, (size_t)len);
    }
    return esp_wifi_internal_tx(0, frame, (uint16_t)(ETH_HDR + 20 + len));
}

/* ---- ARP ------------------------------------------------------------- */

static int send_arp(uint8_t *frame, const uint8_t *dst_mac, uint32_t dst_ip,
                    uint16_t oper)
{
    uint8_t *p = frame + ETH_HDR;
    struct arp_hdr *a = (struct arp_hdr *)p;

    memset(a, 0, sizeof(*a));
    wr16(&a->htype, 1);
    wr16(&a->ptype, ETH_IP);
    a->hlen = 6;
    a->plen = 4;
    wr16(&a->oper, oper);
    memcpy(a->sha, ns.mac, 6);
    wr32((uint8_t *)a + 14, ns.ip);
    memcpy(a->tha, dst_mac, 6);
    wr32((uint8_t *)a + 24, dst_ip);
    return send_eth(frame, dst_mac, ETH_ARP, p, sizeof(*a));
}

static int arp_resolve(uint32_t ip, uint8_t mac[6], int timeout_ms)
{
    int waited = 0;

    ns.arp_want_ip = ip;
    ns.arp_got = 0;
    send_arp(tx_frame, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", ip, 1);
    while (waited < timeout_ms) {
        process_ktask_sleep(50);
        waited += 50;
        if (ns.arp_got && ns.arp_want_ip == ip) {
            memcpy(mac, (const void *)ns.arp_mac, 6);
            return 0;
        }
        if ((waited % 250) == 0)
            send_arp(tx_frame, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", ip, 1);
    }
    return -1;
}

/* ---- DHCP ------------------------------------------------------------ */

static int dhcp_tx(int type, uint32_t ciaddr, uint32_t requested,
                   uint32_t server, int broadcast, const uint8_t *dst_mac)
{
    int n = 0, opt = 0;
    uint8_t *p = dhcp_pkt;
    uint32_t dst_ip = broadcast ? 0xffffffffu : server;

    memset(dhcp_pkt, 0, sizeof(dhcp_pkt));
    p[0] = 1;                   /* BOOTREQUEST */
    p[1] = 1;                   /* Ethernet */
    p[2] = 6;                   /* hlen */
    wr32(p + 4, ns.dhcp_xid);
    wr16(p + 10, broadcast ? 0x8000 : 0);
    wr32(p + 12, ciaddr);       /* ciaddr for RENEWING/REBINDING */
    memcpy(p + 28, ns.mac, 6);  /* chaddr */
    wr32(p + 236, 0x63825363);  /* magic cookie */

    opt = 240;
    p[opt++] = 53;
    p[opt++] = 1;
    p[opt++] = (uint8_t)type;
    p[opt++] = 61;
    p[opt++] = 7;
    p[opt++] = 1;
    memcpy(p + opt, ns.mac, 6);
    opt += 6;
    /* Requested IP and server identifier belong to SELECTING only; a
     * RENEWING/REBINDING request carries ciaddr instead. */
    if (type == 3 && requested && ciaddr == 0) {
        p[opt++] = 50;
        p[opt++] = 4;
        wr32(p + opt, requested);
        opt += 4;
        if (server) {
            p[opt++] = 54;
            p[opt++] = 4;
            wr32(p + opt, server);
            opt += 4;
        }
    }
    p[opt++] = 55;
    p[opt++] = 4;
    p[opt++] = 1;
    p[opt++] = 3;
    p[opt++] = 6;
    p[opt++] = 15;
    p[opt++] = 255;
    n = opt < 300 ? 300 : opt;

    {
        uint8_t udp[8 + 320];
        struct udp_hdr *u = (struct udp_hdr *)udp;

        wr16(&u->sport, DHCP_CLIENT_PORT);
        wr16(&u->dport, DHCP_SERVER_PORT);
        wr16(&u->len, (uint16_t)(8 + n));
        wr16(&u->check, 0);
        memcpy(udp + 8, dhcp_pkt, (size_t)n);
        return transmit_ip(broadcast ? (const uint8_t *)"\xff\xff\xff\xff\xff\xff"
                                      : dst_mac,
                           dst_ip, IP_UDP, udp, 8 + n, 0);
    }
}

static void dhcp_parse(const uint8_t *p, int len)
{
    int opt;
    uint8_t type = 0;
    uint32_t yi = 0, mask = 0, gw = 0, dns = 0, server = 0;
    uint32_t lease = 0, t1 = 0, t2 = 0;

    if (len < 240 || p[0] != 2 || rd32(p + 4) != ns.dhcp_xid)
        return;
    if (rd32(p + 236) != 0x63825363)
        return;
    yi = rd32(p + 16);
    opt = 240;
    while (opt + 1 < len) {
        uint8_t c = p[opt];
        uint8_t l = p[opt + 1];

        if (c == 255)
            break;
        if (c == 0) {
            opt++;
            continue;
        }
        if (opt + 2 + l > len)
            break;
        switch (c) {
        case 53:
            if (l >= 1)
                type = p[opt + 2];
            break;
        case 1:
            if (l >= 4)
                mask = rd32(p + opt + 2);
            break;
        case 3:
            if (l >= 4)
                gw = rd32(p + opt + 2);
            break;
        case 6:
            if (l >= 4)
                dns = rd32(p + opt + 2);
            break;
        case 51:
            if (l >= 4)
                lease = rd32(p + opt + 2);
            break;
        case 54:
            if (l >= 4)
                server = rd32(p + opt + 2);
            break;
        case 58:
            if (l >= 4)
                t1 = rd32(p + opt + 2);
            break;
        case 59:
            if (l >= 4)
                t2 = rd32(p + opt + 2);
            break;
        }
        opt += 2 + l;
    }
    ns.dhcp_yiaddr = yi;
    ns.dhcp_mask = mask;
    ns.dhcp_gw = gw;
    ns.dhcp_dns = dns;
    ns.dhcp_server = server;
    if (lease)
        ns.lease = lease;
    if (t1)
        ns.dhcp_t1_s = t1;
    if (t2)
        ns.dhcp_t2_s = t2;
    ns.dhcp_rx_type = type;
}

/* ---- DNS ------------------------------------------------------------- */

static int dns_skip_name(const uint8_t *p, int len, int off)
{
    while (off < len) {
        uint8_t b = p[off];

        if (b == 0)
            return off + 1;
        if ((b & 0xc0) == 0xc0)
            return off + 2;
        off += 1 + b;
    }
    return -1;
}

static void dns_parse(const uint8_t *p, int len)
{
    int off, qd, an, i;

    if (len < 12 || rd16(p) != ns.dns_id || !(p[2] & 0x80))
        return;
    qd = rd16(p + 4);
    an = rd16(p + 6);
    off = 12;
    for (i = 0; i < qd; i++) {
        off = dns_skip_name(p, len, off);
        if (off < 0 || off + 4 > len)
            return;
        off += 4;
    }
    for (i = 0; i < an; i++) {
        uint16_t type, class_, rdlen;

        off = dns_skip_name(p, len, off);
        if (off < 0 || off + 10 > len)
            return;
        type = rd16(p + off);
        class_ = rd16(p + off + 2);
        rdlen = rd16(p + off + 8);
        off += 10;
        if (off + rdlen > len)
            return;
        if (type == 1 && class_ == 1 && rdlen == 4) {
            ns.dns_result = rd32(p + off);
            ns.dns_ok = 1;
            break;
        }
        off += rdlen;
    }
    ns.dns_done = 1;
}

static int send_udp(uint32_t dst_ip, const uint8_t *dst_mac, uint16_t sport,
                    uint16_t dport, const uint8_t *payload, int len)
{
    uint8_t buf[8 + 256];
    struct udp_hdr *u = (struct udp_hdr *)buf;

    if (len < 0 || len > 248)
        return -1;
    wr16(&u->sport, sport);
    wr16(&u->dport, dport);
    wr16(&u->len, (uint16_t)(8 + len));
    wr16(&u->check, 0);
    memcpy(buf + 8, payload, (size_t)len);
    return transmit_ip(dst_mac, dst_ip, IP_UDP, buf, 8 + len, 0);
}

static int dns_resolve(const char *name, uint32_t *out)
{
    uint8_t q[128];
    int off = 12;
    uint16_t id;
    const char *s = name;

    if (!ns.bound || !ns.dns)
        return -1;
    memset(q, 0, sizeof(q));
    id = ++ns.dns_next_id;
    if (!id)
        id = ++ns.dns_next_id;
    wr16(q, id);
    q[2] = 0x01;                /* recursion desired */
    wr16(q + 4, 1);             /* qdcount */
    for (;;) {
        int n = 0;

        while (*s && *s != '.' && n < 63) {
            q[off + 1 + n] = (uint8_t)*s++;
            n++;
        }
        if (n == 0 || off + 2 + n >= (int)sizeof(q))
            return -1;
        q[off] = (uint8_t)n;
        off += 1 + n;
        if (*s == '.') {
            s++;
            continue;
        }
        break;
    }
    q[off++] = 0;
    if (off + 4 > (int)sizeof(q))
        return -1;
    wr16(q + off, 1);           /* QTYPE A */
    wr16(q + off + 2, 1);       /* QCLASS IN */
    off += 4;

    ns.dns_id = id;
    ns.dns_done = 0;
    ns.dns_ok = 0;
    if (!ns.dns_mac_valid) {
        if (arp_resolve(ns.dns, ns.dns_mac, 500) == 0)
            ns.dns_mac_valid = 1;
        else
            return -1;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        int waited = 0;

        send_udp(ns.dns, ns.dns_mac, DNS_LOCAL_PORT, DNS_PORT, q, off);
        while (waited < 1500) {
            process_ktask_sleep(50);
            waited += 50;
            if (ns.dns_done)
                break;
        }
        if (ns.dns_done) {
            if (ns.dns_ok) {
                *out = ns.dns_result;
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

/* ---- TCP + HTTP ------------------------------------------------------ */

static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *tcp,
                             int len)
{
    uint8_t ph[12];
    uint32_t sum = 0;

    wr32(ph, src);
    wr32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IP_TCP;
    wr16(ph + 10, (uint16_t)len);
    sum = csum_add(sum, ph, 12);
    sum = csum_add(sum, tcp, len);
    return csum_fin(sum);
}

static int tcp_tx_at(uint32_t seq, uint32_t ack, uint8_t flags,
                     const uint8_t *data, int len)
{
    uint8_t buf[20 + 256];
    struct tcp_hdr *t = (struct tcp_hdr *)buf;

    if (len < 0 || len > 256)
        return -1;
    memset(t, 0, 20);
    wr16(&t->sport, ns.tcp_local_port);
    wr16(&t->dport, ns.tcp_remote_port);
    wr32(&t->seq, seq);
    wr32(&t->ack, ack);
    t->off = 5 << 4;
    t->flags = flags;
    wr16(&t->win, 2048);
    wr16(&t->urg, 0);
    if (len)
        memcpy(buf + 20, data, (size_t)len);
    wr16(&t->check, 0);
    wr16(&t->check, tcp_checksum(ns.ip, ns.tcp_remote_ip, buf, 20 + len));
    return transmit_ip(ns.tcp_mac, ns.tcp_remote_ip, IP_TCP, buf, 20 + len, 0);
}

static int tcp_send_data(const uint8_t *data, int len)
{
    int rc = tcp_tx_at(ns.tcp_snd_nxt, ns.tcp_rcv_nxt, TCP_PSH | TCP_ACK,
                       data, len);

    if (rc == 0)
        ns.tcp_snd_nxt += (uint32_t)len;
    return rc;
}

static void tcp_send_fin(void)
{
    tcp_tx_at(ns.tcp_snd_nxt, ns.tcp_rcv_nxt, TCP_FIN | TCP_ACK, NULL, 0);
    ns.tcp_snd_nxt++;
}

static void tcp_input(uint32_t sip, const uint8_t *payload, int len)
{
    const struct tcp_hdr *t = (const struct tcp_hdr *)payload;
    uint16_t sport, dport;
    uint8_t flags;
    uint32_t seq, ack;
    int hlen, full;

    if (len < 20 || sip != ns.tcp_remote_ip) {
        ns.rx_drop++;
        return;
    }
    sport = rd16(&t->sport);
    dport = rd16(&t->dport);
    if (sport != ns.tcp_remote_port || dport != ns.tcp_local_port) {
        ns.rx_drop++;
        return;
    }
    hlen = (t->off >> 4) * 4;
    if (hlen < 20 || hlen > len)
        return;
    if (tcp_checksum(sip, ns.ip, payload, len) != 0) {
        ns.rx_drop++;
        return;
    }
    flags = t->flags;
    seq = rd32(&t->seq);
    ack = rd32(&t->ack);

    if (flags & TCP_RST) {
        ns.tcp_got_rst = 1;
        ns.tcp_state = TCP_DONE;
        return;
    }
    if ((flags & TCP_SYN) && (flags & TCP_ACK) &&
        ns.tcp_state == TCP_SYN_SENT) {
        ns.tcp_rcv_nxt = seq + 1;
        if ((int32_t)(ack - ns.tcp_snd_una) > 0)
            ns.tcp_snd_una = ack;
        ns.tcp_got_synack = 1;
        return;
    }
    if (flags & TCP_ACK) {
        if ((int32_t)(ack - ns.tcp_snd_una) > 0)
            ns.tcp_snd_una = ack;
        ns.tcp_got_ack = 1;
    }
    full = len - hlen;
    if (full > 0) {
        if (seq == ns.tcp_rcv_nxt) {
            int room = (int)sizeof(ns.tcp_rx_buf) - ns.tcp_rx_len;
            int copy = full > room ? room : full;

            if (copy > 0) {
                memcpy(ns.tcp_rx_buf + ns.tcp_rx_len, payload + hlen,
                       (size_t)copy);
                ns.tcp_rx_len += copy;
            }
            if (copy < full)
                ns.tcp_rx_overflow = 1;
            ns.tcp_rcv_nxt += (uint32_t)full;
        }
        ns.tcp_got_data = 1;
    }
    if (flags & TCP_FIN) {
        ns.tcp_rcv_nxt++;
        ns.tcp_got_fin = 1;
    }
}

static int http_ci_eq(const uint8_t *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t x = a[i];
        uint8_t y = (uint8_t)b[i];

        if (x >= 'A' && x <= 'Z')
            x = (uint8_t)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (uint8_t)(y + 32);
        if (x != y || !y)
            return 0;
    }
    return b[n] == 0;
}

static int http_header(const uint8_t *p, int hdr_end, const char *name,
                       char *out, int cap)
{
    int i = 0;

    while (i + 1 < hdr_end && !(p[i] == '\r' && p[i + 1] == '\n'))
        i++;
    i += 2;
    while (i < hdr_end) {
        int e = i, colon;

        while (e + 1 < hdr_end && !(p[e] == '\r' && p[e + 1] == '\n'))
            e++;
        colon = i;
        while (colon < e && p[colon] != ':')
            colon++;
        if (colon < e && http_ci_eq(p + i, name, colon - i)) {
            int v = colon + 1, k = 0;

            while (v < e && (p[v] == ' ' || p[v] == '\t'))
                v++;
            while (v < e && k < cap - 1)
                out[k++] = (char)p[v++];
            out[k] = 0;
            return 1;
        }
        i = e + 2;
    }
    return 0;
}

static int http_dechunk(const uint8_t *in, int len, uint8_t *out, int cap)
{
    int i = 0, o = 0;

    while (i < len) {
        int sz = 0, digits = 0;

        for (;;) {
            uint8_t c = in[i];
            int v;

            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else
                break;
            sz = sz * 16 + v;
            i++;
            digits++;
            if (i >= len)
                break;
        }
        if (!digits)
            break;
        while (i < len && (in[i] == '\r' || in[i] == '\n'))
            i++;
        if (sz == 0)
            break;
        if (i + sz > len)
            sz = len - i;
        for (int k = 0; k < sz && o < cap; k++)
            out[o++] = in[i + k];
        i += sz;
        while (i < len && (in[i] == '\r' || in[i] == '\n'))
            i++;
    }
    return o;
}

static void http_emit(void)
{
    static uint8_t body_buf[512];
    int len = ns.tcp_rx_len;
    int hdr_end = -1, body = 0, blen = 0;
    int chunked = 0, content_len = 0;
    char val[96];

    kprintf("[net] http rx %d bytes%s\n", len,
            ns.tcp_rx_overflow ? " (truncated)" : "");
    for (int i = 0; i + 3 < len; i++) {
        if (ns.tcp_rx_buf[i] == '\r' && ns.tcp_rx_buf[i + 1] == '\n' &&
            ns.tcp_rx_buf[i + 2] == '\r' && ns.tcp_rx_buf[i + 3] == '\n') {
            hdr_end = i;
            body = i + 4;
            break;
        }
    }
    if (hdr_end < 0) {
        kprintf("[net] (no complete header)\n");
        return;
    }
    {
        int e = 0;

        while (e < hdr_end && ns.tcp_rx_buf[e] != '\r' &&
               ns.tcp_rx_buf[e] != '\n')
            e++;
        kprintf("[net] ");
        for (int i = 0; i < e && i < 90; i++) {
            uint8_t c = ns.tcp_rx_buf[i];

            kprintf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        kprintf("\n");
    }
    if (http_header(ns.tcp_rx_buf, hdr_end, "content-type", val,
                    sizeof(val)))
        kprintf("[net] content-type: %s\n", val);
    if (http_header(ns.tcp_rx_buf, hdr_end, "content-length", val,
                    sizeof(val))) {
        for (int i = 0; val[i] >= '0' && val[i] <= '9'; i++)
            content_len = content_len * 10 + (val[i] - '0');
        kprintf("[net] content-length: %d\n", content_len);
    }
    if (http_header(ns.tcp_rx_buf, hdr_end, "transfer-encoding", val,
                    sizeof(val))) {
        kprintf("[net] transfer-encoding: %s\n", val);
        if (val[0] == 'c' || val[0] == 'C')
            chunked = 1;
    }
    if (http_header(ns.tcp_rx_buf, hdr_end, "location", val, sizeof(val)))
        kprintf("[net] location: %s\n", val);

    blen = len - body;
    if (blen < 0)
        blen = 0;
    if (chunked) {
        blen = http_dechunk(ns.tcp_rx_buf + body, blen, body_buf,
                            sizeof(body_buf));
        if (blen > 0)
            kprintf("[net] body (%d decoded): ", blen);
    } else {
        if (content_len && content_len < blen)
            blen = content_len;
        if (blen > 0)
            kprintf("[net] body (%d): ", blen);
    }
    if (blen > 0) {
        int show = blen > 400 ? 400 : blen;

        for (int i = 0; i < show; i++) {
            uint8_t c = chunked ? body_buf[i] : ns.tcp_rx_buf[body + i];

            kprintf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        kprintf("\n");
    }
}

static void http_run(const char *host, uint32_t ip, uint16_t port,
                     const char *path)
{
    uint32_t hop;
    int n, tries, waited, idle;
    char pathbuf[64];

    if (!ns.bound) {
        kprintf("[net] http: no IP address\n");
        return;
    }
    /* net_write() runs in the caller's context, so keep a private copy of the
     * path in case the shell starts another request while this one runs. */
    {
        int i = 0;

        while (path[i] && i < (int)sizeof(pathbuf) - 1) {
            pathbuf[i] = path[i];
            i++;
        }
        pathbuf[i] = 0;
        path = pathbuf;
    }
    hop = ((ip ^ ns.ip) & ns.mask) ? ns.gw : ip;
    if (arp_resolve(hop, ns.tcp_mac, 1500) != 0) {
        kprintf("[net] http: arp timeout\n");
        return;
    }
    ns.tcp_remote_ip = ip;
    ns.tcp_remote_port = port;
    ns.tcp_local_port = (uint16_t)(40000 + (plat_uptime_ms() & 0x3fff));
    ns.tcp_iss = plat_uptime_ms() * 2654435761u + 0x9e3779b9u;
    ns.tcp_snd_nxt = ns.tcp_iss;
    ns.tcp_snd_una = ns.tcp_iss;
    ns.tcp_rcv_nxt = 0;
    ns.tcp_rx_len = 0;
    ns.tcp_rx_overflow = 0;
    ns.tcp_got_synack = ns.tcp_got_ack = ns.tcp_got_data = 0;
    ns.tcp_got_fin = ns.tcp_got_rst = 0;
    ns.tcp_state = TCP_SYN_SENT;

    for (tries = 0; tries < 3; tries++) {
        tcp_tx_at(ns.tcp_iss, 0, TCP_SYN, NULL, 0);
        waited = 0;
        while (waited < 1000) {
            process_ktask_sleep(50);
            waited += 50;
            if (ns.tcp_got_synack || ns.tcp_got_rst)
                break;
        }
        if (ns.tcp_got_synack || ns.tcp_got_rst)
            break;
    }
    if (!ns.tcp_got_synack || ns.tcp_got_rst) {
        kprintf("[net] http: connect failed\n");
        ns.tcp_state = TCP_DONE;
        return;
    }
    ns.tcp_snd_nxt = ns.tcp_iss + 1;
    tcp_tx_at(ns.tcp_snd_nxt, ns.tcp_rcv_nxt, TCP_ACK, NULL, 0);
    ns.tcp_state = TCP_ESTAB;
    kprintf("[net] http: connected %u.%u.%u.%u:%u\n",
            (unsigned)((ip >> 24) & 255), (unsigned)((ip >> 16) & 255),
            (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255),
            (unsigned)port);

    n = ksnprintf((char *)ns.http_req, sizeof(ns.http_req),
                  "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: TinyOS\r\n"
                  "Connection: close\r\n\r\n",
                  path, host);
    if (n <= 0) {
        ns.tcp_state = TCP_DONE;
        return;
    }
    ns.http_req_len = n;
    ns.http_req_seq = ns.tcp_snd_nxt;
    ns.http_req_retx = 0;
    ns.http_req_next = plat_uptime_ms() + 1000;
    tcp_send_data(ns.http_req, n);

    idle = 0;
    for (int t = 0; t < 300; t++) {
        uint32_t now = plat_uptime_ms();

        if (ns.tcp_got_rst)
            break;
        if (ns.tcp_got_data) {
            ns.tcp_got_data = 0;
            tcp_tx_at(ns.tcp_snd_nxt, ns.tcp_rcv_nxt, TCP_ACK, NULL, 0);
            idle = 0;
        }
        if (ns.tcp_got_fin) {
            tcp_tx_at(ns.tcp_snd_nxt, ns.tcp_rcv_nxt, TCP_ACK, NULL, 0);
            break;
        }
        if (ns.tcp_snd_una != ns.tcp_snd_nxt && ns.http_req_retx < 3 &&
            (int32_t)(now - ns.http_req_next) >= 0) {
            tcp_tx_at(ns.http_req_seq, ns.tcp_rcv_nxt, TCP_PSH | TCP_ACK,
                      ns.http_req, ns.http_req_len);
            ns.http_req_next = now + 1000;
            ns.http_req_retx++;
        }
        idle++;
        if (idle > 40 && ns.tcp_rx_len > 0)
            break;
        process_ktask_sleep(50);
    }
    if (ns.tcp_got_rst)
        kprintf("[net] http: reset\n");
    http_emit();
    if (!ns.tcp_got_rst)
        tcp_send_fin();
    ns.tcp_state = TCP_DONE;
}

/* ---- ICMP ------------------------------------------------------------ */

static uint16_t ping_payload(const uint8_t *p, int len)
{
    return ip_checksum(p, len);
}

static void icmp_reply(const uint8_t *frame, int len)
{
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    const struct ip_hdr *ip = (const struct ip_hdr *)(frame + ETH_HDR);
    const struct icmp_hdr *ic = (const struct icmp_hdr *)(frame + ETH_HDR + 20);
    struct icmp_hdr *out = (struct icmp_hdr *)rx_payload;
    int plen = len - ETH_HDR - 20;

    if (plen < 8 || plen > MTU)
        return;
    memcpy(rx_payload, ic, (size_t)plen);
    out->type = 0;
    out->code = 0;
    wr16(&out->check, 0);
    wr16(&out->check, ping_payload(rx_payload, plen));
    transmit_ip(e->src, rd32((const uint8_t *)ip + 12), IP_ICMP,
                rx_payload, plen, 1);
}

static int ping_send(uint32_t ip, const uint8_t *mac, uint16_t seq)
{
    uint8_t buf[64];
    struct icmp_hdr *ic = (struct icmp_hdr *)buf;

    memset(buf, 0, sizeof(buf));
    ic->type = 8;
    ic->code = 0;
    wr16(&ic->id, ns.ping_id);
    wr16(&ic->seq, seq);
    for (int i = 8; i < 40; i++)
        buf[i] = (uint8_t)(i * 7);
    wr16(&ic->check, 0);
    wr16(&ic->check, ping_payload(buf, 40));
    ns.ping_sent_ms = plat_uptime_ms();
    ns.ping_reply_seq = 0;
    return transmit_ip(mac, ip, IP_ICMP, buf, 40, 0);
}

static void ping_run(uint32_t ip, int count)
{
    uint8_t mac[6];
    int got = 0;
    uint32_t hop;

    if (!ns.bound) {
        kprintf("[net] ping: no IP address (dhcp not bound)\n");
        return;
    }
    if (count <= 0)
        count = 4;
    /* Off-subnet destinations go to the gateway's MAC. */
    hop = ((ip ^ ns.ip) & ns.mask) ? ns.gw : ip;
    if (arp_resolve(hop, mac, 1000) != 0) {
        kprintf("[net] ping: arp timeout for %u.%u.%u.%u\n",
                (unsigned)((ip >> 24) & 255), (unsigned)((ip >> 16) & 255),
                (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
        return;
    }
    ns.ping_id = (uint16_t)(plat_uptime_ms() & 0xffff);
    for (int seq = 1; seq <= count; seq++) {
        int waited = 0;

        if (!ns.bound)
            break;
        ping_send(ip, mac, (uint16_t)seq);
        while (waited < 1500) {
            process_ktask_sleep(50);
            waited += 50;
            if (ns.ping_reply_seq == (uint16_t)seq) {
                got++;
                kprintf("[net] %u.%u.%u.%u: seq=%d time=%u ms\n",
                        (unsigned)((ip >> 24) & 255), (unsigned)((ip >> 16) & 255),
                        (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255),
                        seq, (unsigned)ns.ping_rtt);
                break;
            }
        }
        if (ns.ping_reply_seq != (uint16_t)seq)
            kprintf("[net] %u.%u.%u.%u: seq=%d timeout\n",
                    (unsigned)((ip >> 24) & 255), (unsigned)((ip >> 16) & 255),
                    (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), seq);
        if (seq != count)
            process_ktask_sleep(750);
    }
    kprintf("[net] ping: %d/%d replies\n", got, count);
}

/* ---- receive --------------------------------------------------------- */

static int net_rx(void *buffer, uint16_t len, void *eb)
{
    const uint8_t *frame = buffer;
    const struct eth_hdr *e;
    uint16_t type;

    ns.rx_frames++;
    if (len < ETH_HDR) {
        esp_wifi_internal_free_rx_buffer(eb);
        return 0;
    }
    e = (const struct eth_hdr *)frame;
    type = rd16(&e->type);

    if (type == ETH_ARP && len >= ETH_HDR + (int)sizeof(struct arp_hdr)) {
        const struct arp_hdr *a = (const struct arp_hdr *)(frame + ETH_HDR);
        uint16_t op = rd16(&a->oper);
        uint32_t spa = rd32((const uint8_t *)a + 14);
        uint32_t tpa = rd32((const uint8_t *)a + 24);

        if (op == 1 && tpa == ns.ip && ns.bound) {
            uint8_t reply[ETH_HDR + sizeof(struct arp_hdr)];
            struct arp_hdr *r = (struct arp_hdr *)(reply + ETH_HDR);

            memset(r, 0, sizeof(*r));
            wr16(&r->htype, 1);
            wr16(&r->ptype, ETH_IP);
            r->hlen = 6;
            r->plen = 4;
            wr16(&r->oper, 2);
            memcpy(r->sha, ns.mac, 6);
            wr32((uint8_t *)r + 14, ns.ip);
            memcpy(r->tha, a->sha, 6);
            wr32((uint8_t *)r + 24, spa);
            send_eth(rx_frame, e->src, ETH_ARP, (uint8_t *)r, sizeof(*r));
        } else if (op == 2) {
            if (spa == ns.arp_want_ip) {
                memcpy((void *)ns.arp_mac, a->sha, 6);
                ns.arp_got = 1;
            }
        }
    } else if (type == ETH_IP && len >= ETH_HDR + 20) {
        const struct ip_hdr *ip = (const struct ip_hdr *)(frame + ETH_HDR);
        int ihl = (ip->vihl & 0x0f) * 4;
        int tot = rd16(&ip->total);
        uint32_t dip = rd32((const uint8_t *)ip + 16);
        const uint8_t *payload;

        if (ihl < 20 || tot < ihl || ETH_HDR + tot > len)
            goto out;
        payload = frame + ETH_HDR + ihl;
        if (dip != ns.ip && dip != 0xffffffffu)
            goto out;
        ns.rx_ip++;
        if (ip->proto == IP_ICMP && tot - ihl >= 8) {
            const struct icmp_hdr *ic = (const struct icmp_hdr *)payload;

            ns.rx_icmp++;
            if (ic->type == 8)
                icmp_reply(frame, len);
            else if (ic->type == 0 && rd16(&ic->id) == ns.ping_id) {
                ns.ping_rtt = plat_uptime_ms() - ns.ping_sent_ms;
                ns.ping_reply_seq = rd16(&ic->seq);
            }
        } else if (ip->proto == IP_UDP && tot - ihl >= 8) {
            const struct udp_hdr *u = (const struct udp_hdr *)payload;
            uint16_t dport = rd16(&u->dport);

            ns.rx_udp++;
            if (dport == DHCP_CLIENT_PORT && tot - ihl >= 8 + 240)
                dhcp_parse(payload + 8, tot - ihl - 8);
            else if (dport == DNS_LOCAL_PORT && tot - ihl >= 8 + 12)
                dns_parse(payload + 8, tot - ihl - 8);
        } else if (ip->proto == IP_TCP && tot - ihl >= 20) {
            ns.rx_tcp++;
            tcp_input(rd32((const uint8_t *)ip + 12), payload, tot - ihl);
        }
    }

out:
    esp_wifi_internal_free_rx_buffer(eb);
    return 0;
}

/* ---- control task ---------------------------------------------------- */

static void dhcp_apply_ack(uint32_t now)
{
    uint32_t lease = ns.lease ? ns.lease : 3600;
    uint32_t t1 = ns.dhcp_t1_s ? ns.dhcp_t1_s : lease / 2;
    uint32_t t2 = ns.dhcp_t2_s ? ns.dhcp_t2_s : (lease * 7) / 8;

    if (ns.dhcp_yiaddr)
        ns.ip = ns.dhcp_yiaddr;
    if (ns.dhcp_mask)
        ns.mask = ns.dhcp_mask;
    if (ns.dhcp_gw)
        ns.gw = ns.dhcp_gw;
    if (ns.dhcp_dns)
        ns.dns = ns.dhcp_dns;
    if (t1 < 30)
        t1 = 30;
    if (t2 <= t1)
        t2 = t1 + lease / 4;
    ns.dhcp_renew_at = now + t1 * 1000u;
    ns.dhcp_rebind_at = now + t2 * 1000u;
    ns.dhcp_state = 3;
    ns.dhcp_renewing = 0;
    ns.dns_mac_valid = 0;
    ns.bound = 1;
}

static void dhcp_send_renew(int force_broadcast)
{
    int broadcast = force_broadcast;

    if (!broadcast) {
        if (!ns.dhcp_server_mac_valid) {
            uint8_t mac[6];

            if (ns.dhcp_server &&
                arp_resolve(ns.dhcp_server, mac, 500) == 0) {
                memcpy(ns.dhcp_server_mac, mac, 6);
                ns.dhcp_server_mac_valid = 1;
            } else {
                broadcast = 1;
            }
        }
    }
    ns.dhcp_renewing = broadcast ? 2 : 1;
    dhcp_tx(3, ns.ip, 0, ns.dhcp_server, broadcast,
            broadcast ? NULL : ns.dhcp_server_mac);
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now = plat_uptime_ms();

        if (!ns.bound) {
            if (ns.dhcp_state == 0 && (int32_t)(now - ns.dhcp_next) >= 0) {
                ns.dhcp_xid = dhcp_xid_next++;
                ns.dhcp_server_mac_valid = 0;
                dhcp_tx(1, 0, 0, 0, 1, NULL);
                ns.dhcp_state = 1;
                ns.dhcp_next = now + 2000;
                ns.dhcp_tries = 1;
                kprintf("[net] dhcp discover (xid %08x)\n", (unsigned)ns.dhcp_xid);
            } else if (ns.dhcp_state == 1 &&
                       (int32_t)(now - ns.dhcp_next) >= 0) {
                dhcp_tx(1, 0, 0, 0, 1, NULL);
                ns.dhcp_next = now + 2000;
                ns.dhcp_tries++;
            } else if (ns.dhcp_state == 2 &&
                       (int32_t)(now - ns.dhcp_next) >= 0) {
                dhcp_tx(3, 0, (uint32_t)ns.dhcp_yiaddr,
                        (uint32_t)ns.dhcp_server, 1, NULL);
                ns.dhcp_next = now + 2000;
                ns.dhcp_tries++;
                if (ns.dhcp_tries > 6) {
                    ns.dhcp_state = 0;
                    ns.dhcp_next = now;
                }
            }
            if (ns.dhcp_rx_type == 2 && ns.dhcp_state == 1) {
                ns.dhcp_rx_type = 0;
                dhcp_tx(3, 0, (uint32_t)ns.dhcp_yiaddr,
                        (uint32_t)ns.dhcp_server, 1, NULL);
                ns.dhcp_state = 2;
                ns.dhcp_next = now + 2000;
                kprintf("[net] dhcp offer %u.%u.%u.%u\n",
                        (unsigned)((ns.dhcp_yiaddr >> 24) & 255),
                        (unsigned)((ns.dhcp_yiaddr >> 16) & 255),
                        (unsigned)((ns.dhcp_yiaddr >> 8) & 255),
                        (unsigned)(ns.dhcp_yiaddr & 255));
            } else if (ns.dhcp_rx_type == 5 && ns.dhcp_state >= 1) {
                ns.dhcp_rx_type = 0;
                dhcp_apply_ack(now);
                ns.dhcp_server_mac_valid = 0;
                kprintf("[net] dhcp bound ip=%u.%u.%u.%u gw=%u.%u.%u.%u lease=%us\n",
                        (unsigned)((ns.ip >> 24) & 255),
                        (unsigned)((ns.ip >> 16) & 255),
                        (unsigned)((ns.ip >> 8) & 255),
                        (unsigned)(ns.ip & 255),
                        (unsigned)((ns.gw >> 24) & 255),
                        (unsigned)((ns.gw >> 16) & 255),
                        (unsigned)((ns.gw >> 8) & 255),
                        (unsigned)(ns.gw & 255),
                        (unsigned)(ns.lease ? ns.lease : 3600));
                /* Announce the new address so the router/switch learns the
                 * MAC before the first data frame. */
                send_arp(tx_frame,
                         (const uint8_t *)"\xff\xff\xff\xff\xff\xff",
                         ns.ip, 1);
            } else if (ns.dhcp_rx_type == 6) {
                ns.dhcp_rx_type = 0;
                kprintf("[net] dhcp nak, restarting\n");
                ns.dhcp_state = 0;
                ns.bound = 0;
                ns.dhcp_next = now;
            }
        } else {
            if (ns.dhcp_rx_type == 5) {
                ns.dhcp_rx_type = 0;
                dhcp_apply_ack(now);
                kprintf("[net] dhcp renewed ip=%u.%u.%u.%u lease=%us\n",
                        (unsigned)((ns.ip >> 24) & 255),
                        (unsigned)((ns.ip >> 16) & 255),
                        (unsigned)((ns.ip >> 8) & 255),
                        (unsigned)(ns.ip & 255),
                        (unsigned)(ns.lease ? ns.lease : 3600));
            } else if (ns.dhcp_rx_type == 6) {
                ns.dhcp_rx_type = 0;
                kprintf("[net] dhcp nak, restarting\n");
                ns.bound = 0;
                ns.dhcp_state = 0;
                ns.dhcp_next = now;
            } else if (ns.dhcp_renewing == 0 &&
                       (int32_t)(now - ns.dhcp_rebind_at) >= 0) {
                ns.dhcp_xid = dhcp_xid_next++;
                ns.dhcp_tries = 1;
                ns.dhcp_next = now + 4000;
                dhcp_send_renew(1);
                kprintf("[net] dhcp rebind\n");
            } else if (ns.dhcp_renewing == 0 &&
                       (int32_t)(now - ns.dhcp_renew_at) >= 0) {
                ns.dhcp_xid = dhcp_xid_next++;
                ns.dhcp_tries = 1;
                ns.dhcp_next = now + 4000;
                dhcp_send_renew(0);
                kprintf("[net] dhcp renew\n");
            } else if (ns.dhcp_renewing &&
                       (int32_t)(now - ns.dhcp_next) >= 0) {
                ns.dhcp_tries++;
                if (ns.dhcp_tries <= 4) {
                    dhcp_send_renew(ns.dhcp_renewing == 2);
                    ns.dhcp_next = now + 4000;
                } else {
                    /* Lease expired without a server: start over. */
                    kprintf("[net] dhcp lease lost\n");
                    ns.bound = 0;
                    ns.dhcp_state = 0;
                    ns.dhcp_renewing = 0;
                    ns.dhcp_next = now;
                }
            }
        }
        if (ns.http_pending) {
            uint32_t ip = ns.http_ip;
            char host[64];

            ns.http_pending = 0;
            memcpy(host, ns.http_host, sizeof(host));
            host[sizeof(host) - 1] = 0;
            if (ns.http_host_is_ip ||
                dns_resolve(host, &ip) == 0) {
                if (!ns.http_host_is_ip)
                    kprintf("[net] %s -> %u.%u.%u.%u\n", host,
                            (unsigned)((ip >> 24) & 255),
                            (unsigned)((ip >> 16) & 255),
                            (unsigned)((ip >> 8) & 255),
                            (unsigned)(ip & 255));
                http_run(host, ip, ns.http_port, ns.http_path);
            } else {
                kprintf("[net] %s: lookup failed\n", host);
            }
        }
        if (ns.lookup_pending) {
            uint32_t ip;

            ns.lookup_pending = 0;
            if (dns_resolve(ns.lookup_name, &ip) == 0)
                kprintf("[net] %s -> %u.%u.%u.%u\n", ns.lookup_name,
                        (unsigned)((ip >> 24) & 255),
                        (unsigned)((ip >> 16) & 255),
                        (unsigned)((ip >> 8) & 255),
                        (unsigned)(ip & 255));
            else
                kprintf("[net] %s: lookup failed\n", ns.lookup_name);
        }
        if (ns.ping_host_pending) {
            uint32_t ip;
            char host[64];
            int count = ns.ping_count;

            ns.ping_host_pending = 0;
            memcpy(host, ns.ping_host, sizeof(host));
            host[sizeof(host) - 1] = 0;
            if (dns_resolve(host, &ip) == 0) {
                kprintf("[net] %s -> %u.%u.%u.%u\n", host,
                        (unsigned)((ip >> 24) & 255),
                        (unsigned)((ip >> 16) & 255),
                        (unsigned)((ip >> 8) & 255),
                        (unsigned)(ip & 255));
                ping_run(ip, count);
            } else {
                kprintf("[net] %s: lookup failed\n", host);
            }
        }
        if (ns.ping_pending) {
            uint32_t ip = ns.ping_target;
            int count = ns.ping_count;

            ns.ping_pending = 0;
            ping_run(ip, count);
        }
        process_ktask_sleep(50);
    }
}

/* ---- /dev/net -------------------------------------------------------- */

static int net_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    char line[160];
    int n;

    (void)priv;
    if (off)
        return 0;
    n = ksnprintf(line, sizeof(line),
                  "ip %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u %s\n"
                  "rx frames=%u ip=%u udp=%u icmp=%u tcp=%u drop=%u\n",
                  (unsigned)((ns.ip >> 24) & 255), (unsigned)((ns.ip >> 16) & 255),
                  (unsigned)((ns.ip >> 8) & 255), (unsigned)(ns.ip & 255),
                  (unsigned)((ns.mask >> 24) & 255), (unsigned)((ns.mask >> 16) & 255),
                  (unsigned)((ns.mask >> 8) & 255), (unsigned)(ns.mask & 255),
                  (unsigned)((ns.gw >> 24) & 255), (unsigned)((ns.gw >> 16) & 255),
                  (unsigned)((ns.gw >> 8) & 255), (unsigned)(ns.gw & 255),
                  (unsigned)((ns.dns >> 24) & 255), (unsigned)((ns.dns >> 16) & 255),
                  (unsigned)((ns.dns >> 8) & 255), (unsigned)(ns.dns & 255),
                  ns.bound ? "bound" : "down",
                  (unsigned)ns.rx_frames, (unsigned)ns.rx_ip,
                  (unsigned)ns.rx_udp, (unsigned)ns.rx_icmp,
                  (unsigned)ns.rx_tcp, (unsigned)ns.rx_drop);
    if (n < 0)
        return 0;
    if ((uint32_t)n > len)
        n = (int)len;
    memcpy(buf, line, (size_t)n);
    return n;
}

static int parse_ipv4(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    uint32_t ip = 0;

    for (int part = 0; part < 4; part++) {
        int v = 0, digits = 0;

        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p++ - '0');
            digits++;
            if (v > 255)
                return -1;
        }
        if (!digits)
            return -1;
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3) {
            if (*p != '.')
                return -1;
            p++;
        }
    }
    *pp = p;
    *out = ip;
    return 0;
}

static int copy_token(const char **pp, char *dst, int cap)
{
    const char *p = *pp;
    int n = 0;

    while (*p == ' ')
        p++;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && n < cap - 1)
        dst[n++] = *p++;
    dst[n] = 0;
    *pp = p;
    return n;
}

static void parse_count(const char **pp, int *count)
{
    const char *p = *pp;

    while (*p == ' ')
        p++;
    if (*p >= '0' && *p <= '9')
        *count = 0;
    while (*p >= '0' && *p <= '9')
        *count = *count * 10 + (*p++ - '0');
    *pp = p;
}

static int net_write(void *priv, uint32_t off, const void *buf, uint32_t len)
{
    char cmd[64];
    uint32_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
    char *p;
    uint32_t ip;
    int count = 4;

    (void)priv;
    (void)off;
    memcpy(cmd, buf, (size_t)n);
    cmd[n] = 0;
    p = cmd;
    while (*p == ' ')
        p++;

    if (p[0] == 'p' && p[1] == 'i' && p[2] == 'n' && p[3] == 'g') {
        const char *save;

        p += 4;
        while (*p == ' ')
            p++;
        save = p;
        if (parse_ipv4(&p, &ip) == 0) {
            parse_count(&p, &count);
            ns.ping_target = ip;
            ns.ping_count = count;
            ns.ping_pending = 1;
        } else {
            p = save;
            if (copy_token(&p, ns.ping_host, sizeof(ns.ping_host)) == 0)
                return -1;
            parse_count(&p, &count);
            ns.ping_count = count;
            ns.ping_host_pending = 1;
        }
        return (int)len;
    }
    if (p[0] == 'h' && p[1] == 'o' && p[2] == 's' && p[3] == 't') {
        p += 4;
        if (copy_token(&p, ns.lookup_name, sizeof(ns.lookup_name)) == 0)
            return -1;
        ns.lookup_pending = 1;
        return (int)len;
    }
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p') {
        const char *hp;
        int hn = 0;

        p += 4;
        while (*p == ' ')
            p++;
        if (strncmp(p, "https:", 6) == 0) {
            kprintf("[net] https is not supported (no TLS)\n");
            return (int)len;
        }
        if (strncmp(p, "http://", 7) == 0)
            p += 7;
        else if (p[0] == ':' && p[1] == '/' && p[2] == '/')
            p += 3;
        while (*p && *p != ':' && *p != '/' && *p != ' ' && *p != '\n' &&
               *p != '\r' && hn < (int)sizeof(ns.http_host) - 1)
            ns.http_host[hn++] = *p++;
        ns.http_host[hn] = 0;
        if (hn == 0)
            return -1;
        ns.http_port = 80;
        if (*p == ':') {
            int port = 0;

            p++;
            while (*p >= '0' && *p <= '9')
                port = port * 10 + (*p++ - '0');
            if (port)
                ns.http_port = (uint16_t)port;
        }
        while (*p == ' ')
            p++;
        if (*p == '/') {
            copy_token(&p, ns.http_path, sizeof(ns.http_path));
        } else if (*p >= '0' && *p <= '9') {
            int port = 0;

            while (*p >= '0' && *p <= '9')
                port = port * 10 + (*p++ - '0');
            if (port)
                ns.http_port = (uint16_t)port;
            while (*p == ' ')
                p++;
            if (*p == '/')
                copy_token(&p, ns.http_path, sizeof(ns.http_path));
        } else if (*p && *p != '\n' && *p != '\r') {
            ns.http_path[0] = '/';
            copy_token(&p, ns.http_path + 1, sizeof(ns.http_path) - 1);
        }
        if (!ns.http_path[0]) {
            ns.http_path[0] = '/';
            ns.http_path[1] = 0;
        }
        hp = ns.http_host;
        ns.http_host_is_ip = (parse_ipv4(&hp, &ns.http_ip) == 0 && *hp == 0);
        ns.http_pending = 1;
        return (int)len;
    }
    if (p[0] == 'r' && p[1] == 'e' && p[2] == 'n' && p[3] == 'e' &&
        p[4] == 'w') {
        ns.dhcp_renew_at = plat_uptime_ms();
        return (int)len;
    }
    if (p[0] == 'd' && p[1] == 'h' && p[2] == 'c' && p[3] == 'p') {
        ns.bound = 0;
        ns.dhcp_state = 0;
        ns.dhcp_next = plat_uptime_ms();
        ns.dhcp_rx_type = 0;
        ns.dhcp_renewing = 0;
        ns.lease = 0;
        ns.dhcp_t1_s = 0;
        ns.dhcp_t2_s = 0;
        ns.dhcp_server_mac_valid = 0;
        return (int)len;
    }
    return (int)len;
}

static const struct dev_ops net_ops = {
    .read = net_read,
    .write = net_write,
};

void wifi_net_start(void)
{
    if (!ns.started) {
        ns.started = 1;
        esp_wifi_get_macaddr_internal(0, ns.mac);
        esp_wifi_internal_reg_rxcb(0, net_rx);
        devfs_register("dev/net", &net_ops, 0);
        process_ktask_create("net", net_task, 0, 4096, 0);
        kprintf("[net] ready mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                ns.mac[0], ns.mac[1], ns.mac[2], ns.mac[3], ns.mac[4], ns.mac[5]);
    }
    /* Fresh link (or reconnect): forget the old lease and ask again. */
    ns.bound = 0;
    ns.ip = 0;
    ns.mask = 0;
    ns.gw = 0;
    ns.dns = 0;
    ns.dhcp_state = 0;
    ns.dhcp_rx_type = 0;
    ns.dhcp_tries = 0;
    ns.dhcp_renewing = 0;
    ns.lease = 0;
    ns.dhcp_t1_s = 0;
    ns.dhcp_t2_s = 0;
    ns.dhcp_server_mac_valid = 0;
    ns.dhcp_next = plat_uptime_ms();
}

void wifi_net_stop(void)
{
    ns.bound = 0;
    ns.dhcp_state = 0;
    ns.dhcp_rx_type = 0;
    ns.dhcp_renewing = 0;
    ns.ping_pending = 0;
}

#endif /* TOS_NET */
