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
#define IP_UDP   17

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

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

static int dhcp_send(int type, uint32_t requested, uint32_t server)
{
    int n = 0, opt = 0;
    uint8_t *p = dhcp_pkt;
    uint32_t src = ns.bound ? ns.ip : 0;

    memset(dhcp_pkt, 0, sizeof(dhcp_pkt));
    p[0] = 1;                   /* BOOTREQUEST */
    p[1] = 1;                   /* Ethernet */
    p[2] = 6;                   /* hlen */
    wr32(p + 4, ns.dhcp_xid);
    wr16(p + 10, 0x8000);       /* broadcast */
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
    if (type == 3 && requested) {
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
        return transmit_ip((const uint8_t *)"\xff\xff\xff\xff\xff\xff",
                           0xffffffffu, IP_UDP, udp, 8 + n, 0);
    }
}

static void dhcp_parse(const uint8_t *p, int len)
{
    int opt;
    uint8_t type = 0;
    uint32_t yi = 0, mask = 0, gw = 0, dns = 0, server = 0;

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
        case 54:
            if (l >= 4)
                server = rd32(p + opt + 2);
            break;
        }
        opt += 2 + l;
    }
    ns.dhcp_yiaddr = yi;
    ns.dhcp_mask = mask;
    ns.dhcp_gw = gw;
    ns.dhcp_dns = dns;
    ns.dhcp_server = server;
    ns.dhcp_rx_type = type;
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
        if (ip->proto == IP_ICMP && tot - ihl >= 8) {
            const struct icmp_hdr *ic = (const struct icmp_hdr *)payload;

            if (ic->type == 8)
                icmp_reply(frame, len);
            else if (ic->type == 0 && rd16(&ic->id) == ns.ping_id) {
                ns.ping_rtt = plat_uptime_ms() - ns.ping_sent_ms;
                ns.ping_reply_seq = rd16(&ic->seq);
            }
        } else if (ip->proto == IP_UDP && tot - ihl >= 8) {
            const struct udp_hdr *u = (const struct udp_hdr *)payload;
            uint16_t dport = rd16(&u->dport);

            if (dport == DHCP_CLIENT_PORT && tot - ihl >= 8 + 240)
                dhcp_parse(payload + 8, tot - ihl - 8);
        }
    }

out:
    esp_wifi_internal_free_rx_buffer(eb);
    return 0;
}

/* ---- control task ---------------------------------------------------- */

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now = plat_uptime_ms();

        if (!ns.bound) {
            if (ns.dhcp_state == 0 && (int32_t)(now - ns.dhcp_next) >= 0) {
                ns.dhcp_xid = dhcp_xid_next++;
                dhcp_send(1, 0, 0);
                ns.dhcp_state = 1;
                ns.dhcp_next = now + 2000;
                ns.dhcp_tries = 1;
                kprintf("[net] dhcp discover (xid %08x)\n", (unsigned)ns.dhcp_xid);
            } else if (ns.dhcp_state == 1 &&
                       (int32_t)(now - ns.dhcp_next) >= 0) {
                dhcp_send(1, 0, 0);
                ns.dhcp_next = now + 2000;
                ns.dhcp_tries++;
            } else if (ns.dhcp_state == 2 &&
                       (int32_t)(now - ns.dhcp_next) >= 0) {
                dhcp_send(3, (uint32_t)ns.dhcp_yiaddr,
                          (uint32_t)ns.dhcp_server);
                ns.dhcp_next = now + 2000;
                ns.dhcp_tries++;
                if (ns.dhcp_tries > 6) {
                    ns.dhcp_state = 0;
                    ns.dhcp_next = now;
                }
            }
            if (ns.dhcp_rx_type == 2 && ns.dhcp_state == 1) {
                ns.dhcp_rx_type = 0;
                dhcp_send(3, (uint32_t)ns.dhcp_yiaddr, (uint32_t)ns.dhcp_server);
                ns.dhcp_state = 2;
                ns.dhcp_next = now + 2000;
                kprintf("[net] dhcp offer %u.%u.%u.%u\n",
                        (unsigned)((ns.dhcp_yiaddr >> 24) & 255),
                        (unsigned)((ns.dhcp_yiaddr >> 16) & 255),
                        (unsigned)((ns.dhcp_yiaddr >> 8) & 255),
                        (unsigned)(ns.dhcp_yiaddr & 255));
            } else if (ns.dhcp_rx_type == 5 && ns.dhcp_state >= 1) {
                ns.dhcp_rx_type = 0;
                ns.ip = ns.dhcp_yiaddr;
                ns.mask = ns.dhcp_mask ? ns.dhcp_mask : 0xffffff00u;
                ns.gw = ns.dhcp_gw;
                ns.dns = ns.dhcp_dns;
                ns.dhcp_state = 3;
                ns.bound = 1;
                kprintf("[net] dhcp bound ip=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
                        (unsigned)((ns.ip >> 24) & 255),
                        (unsigned)((ns.ip >> 16) & 255),
                        (unsigned)((ns.ip >> 8) & 255),
                        (unsigned)(ns.ip & 255),
                        (unsigned)((ns.gw >> 24) & 255),
                        (unsigned)((ns.gw >> 16) & 255),
                        (unsigned)((ns.gw >> 8) & 255),
                        (unsigned)(ns.gw & 255));
                /* Announce the new address so the router/switch learns the
                 * MAC before the first data frame. */
                send_arp(tx_frame,
                         (const uint8_t *)"\xff\xff\xff\xff\xff\xff",
                         ns.ip, 1);
            } else if (ns.dhcp_rx_type == 6) {
                ns.dhcp_rx_type = 0;
                kprintf("[net] dhcp nak, restarting\n");
                ns.dhcp_state = 0;
                ns.dhcp_next = now;
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
                  "ip %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u %s\n",
                  (unsigned)((ns.ip >> 24) & 255), (unsigned)((ns.ip >> 16) & 255),
                  (unsigned)((ns.ip >> 8) & 255), (unsigned)(ns.ip & 255),
                  (unsigned)((ns.mask >> 24) & 255), (unsigned)((ns.mask >> 16) & 255),
                  (unsigned)((ns.mask >> 8) & 255), (unsigned)(ns.mask & 255),
                  (unsigned)((ns.gw >> 24) & 255), (unsigned)((ns.gw >> 16) & 255),
                  (unsigned)((ns.gw >> 8) & 255), (unsigned)(ns.gw & 255),
                  (unsigned)((ns.dns >> 24) & 255), (unsigned)((ns.dns >> 16) & 255),
                  (unsigned)((ns.dns >> 8) & 255), (unsigned)(ns.dns & 255),
                  ns.bound ? "bound" : "down");
    if (n < 0)
        return 0;
    if ((uint32_t)n > len)
        n = (int)len;
    memcpy(buf, line, (size_t)n);
    return n;
}

static int net_write(void *priv, uint32_t off, const void *buf, uint32_t len)
{
    char cmd[64];
    uint32_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
    char *p;
    uint32_t ip = 0;
    int count = 4;

    (void)priv;
    (void)off;
    memcpy(cmd, buf, (size_t)n);
    cmd[n] = 0;
    p = cmd;
    while (*p == ' ')
        p++;
    if (p[0] == 'p' && p[1] == 'i' && p[2] == 'n' && p[3] == 'g') {
        p += 4;
        while (*p == ' ')
            p++;
        for (int part = 0; part < 4; part++) {
            int v = 0;

            while (*p >= '0' && *p <= '9')
                v = v * 10 + (*p++ - '0');
            ip = (ip << 8) | (uint32_t)(v & 255);
            if (part < 3) {
                if (*p != '.')
                    return -1;
                p++;
            }
        }
        while (*p == ' ')
            p++;
        if (*p >= '0' && *p <= '9')
            count = 0;
        while (*p >= '0' && *p <= '9')
            count = count * 10 + (*p++ - '0');
        ns.ping_target = ip;
        ns.ping_count = count;
        ns.ping_pending = 1;
        return (int)len;
    }
    if (p[0] == 'd' && p[1] == 'h' && p[2] == 'c' && p[3] == 'p') {
        ns.bound = 0;
        ns.dhcp_state = 0;
        ns.dhcp_next = plat_uptime_ms();
        ns.dhcp_rx_type = 0;
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
    ns.dhcp_next = plat_uptime_ms();
}

void wifi_net_stop(void)
{
    ns.bound = 0;
    ns.dhcp_state = 0;
    ns.dhcp_rx_type = 0;
    ns.ping_pending = 0;
}

#endif /* TOS_NET */
