/* Minimal WPA2-PSK supplicant glue for the Espressif WiFi blobs.
 *
 * The net80211 blobs do not contain the WPA state machine.  They call
 * through a `struct wpa_funcs` table that the (source-built) ESP-IDF
 * supplicant registers with esp_wifi_register_wpa_cb_internal().  Without
 * that table g_ic+436 is NULL: scan rejects every RSN AP ("null
 * wpa_parse_wpa_ie") and cnx_do_handoff returns before the auth frame.
 *
 * This file supplies the table.  Step one parses the AP RSN IE so the
 * connect scan accepts the AP and association can begin; the 4-way
 * handshake follows in wifi_wpa4.c.
 */
#include "types.h"
#include "string.h"
#include "console.h"
#include <stdbool.h>

typedef struct {
    int proto;
    int pairwise_cipher;
    int group_cipher;
    int key_mgmt;
    int capabilities;
    size_t num_pmkid;
    const uint8_t *pmkid;
    int mgmt_group_cipher;
    uint8_t rsnxe_capa;
} wifi_wpa_ie_t;

struct wpa_funcs {
    bool (*wpa_sta_init)(void);
    bool (*wpa_sta_deinit)(void);
    int  (*wpa_sta_connect)(uint8_t *bssid);
    void (*wpa_sta_connected_cb)(uint8_t *bssid);
    void (*wpa_sta_disconnected_cb)(uint8_t reason_code);
    int  (*wpa_sta_rx_eapol)(uint8_t *src_addr, uint8_t *buf, uint32_t len);
    bool (*wpa_sta_in_4way_handshake)(void);
    void *(*wpa_ap_init)(void);
    bool (*wpa_ap_deinit)(void *data);
    bool (*wpa_ap_join)(void **sm, uint8_t *bssid, uint8_t *wpa_ie, uint8_t wpa_ie_len);
    bool (*wpa_ap_remove)(void *sm);
    uint8_t *(*wpa_ap_get_wpa_ie)(uint8_t *len);
    bool (*wpa_ap_rx_eapol)(void *hapd_data, void *sm, uint8_t *data, size_t data_len);
    void (*wpa_ap_get_peer_spp_msg)(void *sm, bool *spp_cap, bool *spp_req);
    char *(*wpa_config_parse_string)(const char *value, size_t *len);
    int  (*wpa_parse_wpa_ie)(const uint8_t *wpa_ie, size_t wpa_ie_len,
                             wifi_wpa_ie_t *data);
    int  (*wpa_config_bss)(uint8_t *bssid);
    int  (*wpa_michael_mic_failure)(uint16_t is_unicast);
    uint8_t *(*wpa3_build_sae_msg)(uint8_t *bssid, uint32_t type, size_t *len);
    int  (*wpa3_parse_sae_msg)(uint8_t *buf, size_t len, uint32_t type,
                               uint16_t status);
    int  (*wpa_sta_rx_mgmt)(uint8_t type, uint8_t *frame, size_t len,
                            uint8_t *sender, int8_t rssi, uint8_t channel,
                            uint64_t current_tsf);
    void (*wpa_config_done)(void);
    bool (*wpa_sta_profile_match)(uint8_t *bssid);
    int  (*wpa_sta_set_ap_rsnxe)(const uint8_t *rsnxe, size_t rsnxe_ie_len);
};

/* Public wifi_cipher_type_t values (esp_wifi_types.h). */
#define CIPHER_NONE      0
#define CIPHER_WEP40     1
#define CIPHER_WEP104    2
#define CIPHER_TKIP      3
#define CIPHER_CCMP      4
#define CIPHER_TKIP_CCMP 5
#define CIPHER_AES_CMAC  6
#define CIPHER_GCMP      8
#define CIPHER_GCMP256   9
#define CIPHER_UNKNOWN   13

/* Hostap WPA_KEY_MGMT_* bits used by the blob's auth-mode mapping. */
#define KM_IEEE8021X      0x0001
#define KM_PSK            0x0002
#define KM_FT_IEEE8021X   0x0020
#define KM_FT_PSK         0x0040
#define KM_IEEE8021X_SHA256 0x0080
#define KM_PSK_SHA256     0x0100
#define KM_SAE            0x0400
#define KM_FT_SAE         0x0800

#define WPA_PROTO_WPA 0x1
#define WPA_PROTO_RSN 0x2

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int cipher_from_suite(const uint8_t *s)
{
    /* OUI 00:0f:ac (IEEE 802.11) followed by the suite type. */
    if (s[0] != 0x00 || s[1] != 0x0f || s[2] != 0xac)
        return CIPHER_UNKNOWN;
    switch (s[3]) {
    case 1: return CIPHER_WEP40;
    case 2: return CIPHER_TKIP;
    case 4: return CIPHER_CCMP;
    case 5: return CIPHER_WEP104;
    case 6: return CIPHER_AES_CMAC;
    case 8: return CIPHER_GCMP;
    case 9: return CIPHER_GCMP256;
    default: return CIPHER_UNKNOWN;
    }
}

static int km_from_suite(const uint8_t *s)
{
    if (s[0] != 0x00 || s[1] != 0x0f || s[2] != 0xac)
        return 0;
    switch (s[3]) {
    case 1: return KM_IEEE8021X;
    case 2: return KM_PSK;
    case 3: return KM_FT_IEEE8021X;
    case 4: return KM_FT_PSK;
    case 5: return KM_IEEE8021X_SHA256;
    case 6: return KM_PSK_SHA256;
    case 8: return KM_SAE;
    case 9: return KM_FT_SAE;
    default: return 0;
    }
}

/* Parse one WPA/RSN information element.  `wpa_ie` points at the element
 * id; the blobs pass the saved IE exactly as it appeared in the beacon. */
static int tos_parse_wpa_ie(const uint8_t *wpa_ie, size_t wpa_ie_len,
                            wifi_wpa_ie_t *out)
{
    const uint8_t *p, *end;
    uint8_t id;
    unsigned len, n, i;
    int pairwise = CIPHER_NONE, group = CIPHER_NONE, akm = 0;
    int mgmt = CIPHER_NONE;

    if (!wpa_ie || !out || wpa_ie_len < 2)
        return -1;

    memset(out, 0, sizeof(*out));

    id = wpa_ie[0];
    len = wpa_ie[1];
    if (len + 2u > wpa_ie_len)
        len = (unsigned)(wpa_ie_len - 2);
    if (id != 48 && id != 221)
        return -1;

    p = wpa_ie + 2;
    end = p + len;

    if (id == 221) {
        /* WPA vendor IE: OUI 00:50:f2, type 1, then version. */
        if (end - p < 6)
            return -1;
        if (p[0] != 0x00 || p[1] != 0x50 || p[2] != 0xf2 || p[3] != 1)
            return -1;
        p += 4;
        out->proto = WPA_PROTO_WPA;
    } else {
        out->proto = WPA_PROTO_RSN;
    }

    if (end - p < 2)
        return -1;
    p += 2;                         /* version */

    if (end - p < 4)
        return -1;
    group = cipher_from_suite(p);
    p += 4;

    if (end - p < 2)
        return -1;
    n = rd16(p);
    p += 2;
    if (end - p < (int)(n * 4))
        return -1;
    for (i = 0; i < n; i++) {
        int c = cipher_from_suite(p);

        /* Prefer CCMP when the AP offers several pairwise suites. */
        if (pairwise == CIPHER_NONE || c == CIPHER_CCMP)
            pairwise = c;
        p += 4;
    }

    if (end - p < 2)
        return -1;
    n = rd16(p);
    p += 2;
    if (end - p < (int)(n * 4))
        return -1;
    for (i = 0; i < n; i++) {
        akm |= km_from_suite(p);
        p += 4;
    }

    if (end - p >= 2) {
        out->capabilities = rd16(p);
        p += 2;
    }

    if (id == 48 && end - p >= 2) {
        n = rd16(p);
        p += 2;
        if (end - p >= (int)(n * 16)) {
            out->num_pmkid = n;
            out->pmkid = n ? p : NULL;
            p += n * 16;
        }
    }

    if (id == 48 && end - p >= 4) {
        mgmt = cipher_from_suite(p);
        p += 4;
    }

    out->group_cipher = group;
    out->pairwise_cipher = pairwise;
    out->key_mgmt = akm;
    out->mgmt_group_cipher = mgmt;
    return 0;
}

/* ---- callback table -------------------------------------------------- */

extern int esp_wifi_register_wpa_cb_internal(struct wpa_funcs *cb);
extern int esp_wifi_sta_connect_internal(uint8_t *bssid);
extern int esp_wifi_set_appie_internal(uint8_t type, uint8_t *ie, uint16_t len,
                                       uint8_t flag);
extern uint8_t *esp_wifi_sta_get_prof_password_internal(void);
extern void esp_wifi_auth_done_internal(void);
extern int esp_wifi_get_macaddr_internal(uint8_t ifx, uint8_t *mac);
extern int esp_wifi_internal_tx(int ifx, void *buf, uint16_t len);
extern int esp_wifi_register_eapol_txdonecb_internal(void (*fn)(uint8_t *, size_t, bool));
extern int esp_wifi_set_sta_key_internal(int alg, uint8_t *addr, int key_idx,
                                         int set_tx, uint8_t *seq,
                                         size_t seq_len, uint8_t *key,
                                         size_t key_len, int key_flag);
extern int osi_get_random(uint8_t *buf, size_t len);

/* Crypto helpers implemented in wifi_crypto.c on the C3 ROM engines. */
extern void wifi_hmac_sha1(const uint8_t *key, uint32_t key_len,
                           const uint8_t *data, uint32_t data_len,
                           uint8_t out[20]);
extern void wifi_pbkdf2_sha1(const char *pass, const char *ssid,
                             unsigned ssid_len, int iterations,
                             uint8_t *buf, unsigned buflen);
extern void wifi_aes_block_enc(const uint8_t key[16], const uint8_t in[16],
                               uint8_t out[16]);
extern void wifi_aes_block_dec(const uint8_t key[16], const uint8_t in[16],
                               uint8_t out[16]);

struct wifi_ssid {
    int len;
    uint8_t ssid[32];
};
extern struct wifi_ssid *esp_wifi_sta_get_prof_ssid_internal(void);

/* WIFI_APPIE_RSN from esp_wifi_driver.h. */
#define WIFI_APPIE_RSN 4

/* RSN IE: version 1, group CCMP, pairwise CCMP, AKM PSK, no PMF. */
static const uint8_t wpa2_rsn_ie[] = {
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
    0x00, 0x00,
};

/* ---- minimal WPA2-PSK 4-way handshake --------------------------------- */

#define KEY_FLAG_RX       0x04
#define KEY_FLAG_TX       0x08
#define KEY_FLAG_GROUP    0x10
#define KEY_FLAG_PAIRWISE 0x20

#define ALG_CCMP 3

#define KI_TYPE_SHA1 0x0002
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENCR      0x1000

static struct {
    uint8_t pmk[32];
    uint8_t ptk[48];            /* KCK 16 | KEK 16 | TK 16 */
    uint8_t snonce[32];
    uint8_t anonce[32];
    uint8_t replay[8];
    uint8_t bssid[6];
    uint8_t own[6];
    uint8_t have_pmk;
    uint8_t pending_ptk;
    uint8_t pending_activate;
    uint8_t phase;              /* 0 idle, 1 wait 3/4, 2 done */
    uint16_t m3_ki;
} ws;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void prf_512(const uint8_t *key, const uint8_t *data, size_t data_len,
                    uint8_t out[48])
{
    static const char label[] = "Pairwise key expansion";
    uint8_t buf[128];
    uint8_t h[20];
    size_t pos = 0;
    uint8_t i = 0;

    while (pos < 48) {
        size_t n = sizeof(label) - 1;

        memcpy(buf, label, n);
        buf[n++] = 0;
        memcpy(buf + n, data, data_len);
        n += data_len;
        buf[n++] = i;
        wifi_hmac_sha1(key, 32, buf, (uint32_t)n, h);
        {
            size_t take = 48 - pos;

            if (take > 20)
                take = 20;
            memcpy(out + pos, h, take);
            pos += take;
        }
        i++;
    }
}

static void send_eapol(uint8_t *body, size_t blen, int with_mic)
{
    uint8_t eapol[4 + 160];
    uint8_t frame[14 + 4 + 160];

    eapol[0] = 1;                      /* 802.1X-2001, as the AP uses */
    eapol[1] = 3;
    put16(eapol + 2, (uint16_t)blen);
    memcpy(eapol + 4, body, blen);
    if (with_mic) {
        uint8_t mic[20];

        memset(eapol + 4 + 77, 0, 16);
        wifi_hmac_sha1(ws.ptk, 16, eapol, (uint32_t)(4 + blen), mic);
        memcpy(eapol + 4 + 77, mic, 16);
    }

    memcpy(frame, ws.bssid, 6);
    memcpy(frame + 6, ws.own, 6);
    frame[12] = 0x88;
    frame[13] = 0x8e;
    memcpy(frame + 14, eapol, 4 + blen);
    {
        int rc = esp_wifi_internal_tx(0, frame, (uint16_t)(14 + 4 + blen));

        if (rc != 0)
            kprintf("[wpa] tx eapol failed rc=%d\n", rc);
    }
}

static void derive_ptk(void)
{
    uint8_t b[76];
    const uint8_t *a = ws.bssid, *s = ws.own;
    const uint8_t *an = ws.anonce, *sn = ws.snonce;

    if (memcmp(a, s, 6) > 0) {
        const uint8_t *t = a;

        a = s;
        s = t;
    }
    memcpy(b, a, 6);
    memcpy(b + 6, s, 6);
    if (memcmp(an, sn, 32) > 0) {
        const uint8_t *t = an;

        an = sn;
        sn = t;
    }
    memcpy(b + 12, an, 32);
    memcpy(b + 44, sn, 32);
    prf_512(ws.pmk, b, sizeof(b), ws.ptk);
}

static int aes_unwrap(const uint8_t *kek, const uint8_t *in, int inlen,
                      uint8_t *out)
{
    static const uint8_t check[8] = {
        0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6
    };
    uint8_t a[8], r[32][8], blk[16];
    int n, i, j, k;

    if (inlen < 16 || (inlen & 7))
        return -1;
    n = inlen / 8 - 1;
    if (n < 1 || n > 32)
        return -1;
    memcpy(a, in, 8);
    for (i = 0; i < n; i++)
        memcpy(r[i], in + 8 + 8 * i, 8);

    for (j = 5; j >= 0; j--) {
        for (i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint64_t)j + (uint64_t)i;

            memcpy(blk, a, 8);
            memcpy(blk + 8, r[i - 1], 8);
            for (k = 0; k < 8; k++)
                blk[k] ^= (uint8_t)(t >> (8 * (7 - k)));
            wifi_aes_block_dec(kek, blk, blk);
            memcpy(a, blk, 8);
            memcpy(r[i - 1], blk + 8, 8);
        }
    }
    if (memcmp(a, check, 8) != 0)
        return -1;
    for (i = 0; i < n; i++)
        memcpy(out + 8 * i, r[i], 8);
    return n * 8;
}

static void send_msg2(void)
{
    uint8_t body[95 + 64];

    memset(body, 0, sizeof(body));
    body[0] = 2;                        /* RSN key descriptor */
    put16(body + 1, KI_TYPE_SHA1 | KI_PAIRWISE | KI_MIC);
    put16(body + 3, 0);
    memcpy(body + 5, ws.replay, 8);
    memcpy(body + 13, ws.snonce, 32);
    put16(body + 93, (uint16_t)sizeof(wpa2_rsn_ie));
    memcpy(body + 95, wpa2_rsn_ie, sizeof(wpa2_rsn_ie));
    send_eapol(body, 95 + sizeof(wpa2_rsn_ie), 1);
}

static void send_msg4(void)
{
    uint8_t body[95];

    memset(body, 0, sizeof(body));
    body[0] = 2;
    put16(body + 1, (uint16_t)((ws.m3_ki & KI_SECURE) | KI_TYPE_SHA1 |
                               KI_PAIRWISE | KI_MIC));
    put16(body + 3, 0);
    memcpy(body + 5, ws.replay, 8);
    put16(body + 93, 0);
    send_eapol(body, 95, 1);
}

static void tos_eapol_txdone(uint8_t *payload, size_t len, bool fail)
{
    (void)payload;
    (void)len;
    if (fail) {
        kprintf("[wpa] eapol tx failed\n");
        return;
    }
    if (ws.pending_ptk) {
        ws.pending_ptk = 0;
        /* RX only: msg4 still has to leave the chip in the clear. */
        esp_wifi_set_sta_key_internal(ALG_CCMP, ws.bssid, 0, 0, NULL, 0,
                                      ws.ptk + 32, 16,
                                      KEY_FLAG_PAIRWISE | KEY_FLAG_RX);
        ws.phase = 1;
    }
    if (ws.pending_activate) {
        ws.pending_activate = 0;
        esp_wifi_set_sta_key_internal(ALG_CCMP, ws.bssid, 0, 1, NULL, 0,
                                      ws.ptk + 32, 16,
                                      KEY_FLAG_PAIRWISE | KEY_FLAG_TX);
        ws.phase = 2;
        esp_wifi_auth_done_internal();
        kprintf("[wpa] handshake done\n");
#ifdef TOS_NET
        {
            extern void wifi_net_start(void);

            wifi_net_start();
        }
#endif
    }
}

static bool wpa_sta_init(void)
{
    extern int esp_wifi_register_eapol_txdonecb_internal(
        void (*fn)(uint8_t *, size_t, bool));
    int rc;

    kprintf("[wpa] sta init\n");
    rc = esp_wifi_register_eapol_txdonecb_internal(tos_eapol_txdone);
    kprintf("[wpa] eapol txcb rc=%d\n", rc);
    return true;
}

static bool wpa_sta_deinit(void)
{
    ws.phase = 0;
    return true;
}

static int wpa_sta_connect(uint8_t *bssid)
{
    struct wifi_ssid *ss = esp_wifi_sta_get_prof_ssid_internal();
    uint8_t *pw = esp_wifi_sta_get_prof_password_internal();
    int rc;

    kprintf("[wpa] sta connect %02x:%02x:%02x:%02x:%02x:%02x\n",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    memcpy(ws.bssid, bssid, 6);
    esp_wifi_get_macaddr_internal(0, ws.own);
    ws.phase = 0;
    ws.have_pmk = 0;
    ws.pending_ptk = 0;
    ws.pending_activate = 0;
    if (ss && pw && pw[0] && ss->len > 0) {
        wifi_pbkdf2_sha1((const char *)pw, (const char *)ss->ssid,
                         (unsigned)ss->len, 4096, ws.pmk, 32);
        ws.have_pmk = 1;
    }
    osi_get_random(ws.snonce, 32);

    /* Advertise the same RSN parameters the AP expects in the assoc
     * request (flag 0: the driver copies and prefixes the length). */
    esp_wifi_set_appie_internal(WIFI_APPIE_RSN, (uint8_t *)wpa2_rsn_ie,
                                sizeof(wpa2_rsn_ie), 0);

    /* The driver hands us the chosen BSS and expects the supplicant to
     * start the state machine once its profile is ready. */
    rc = esp_wifi_sta_connect_internal(bssid);
    return rc;
}

static void wpa_sta_connected_cb(uint8_t *bssid)
{
    (void)bssid;
    kprintf("[wpa] associated pmk=%u\n", (unsigned)ws.have_pmk);
}

static void wpa_sta_disconnected_cb(uint8_t reason)
{
#ifdef TOS_NET
    extern void wifi_net_stop(void);

    wifi_net_stop();
#endif
    kprintf("[wpa] disconnected reason=%u\n", (unsigned)reason);
    ws.phase = 0;
}

static int wpa_sta_rx_eapol(uint8_t *src, uint8_t *buf, uint32_t len)
{
    const uint8_t *e = buf;
    uint32_t n = len;
    uint8_t copy[4 + 160];
    uint16_t blen, ki;
    const uint8_t *body;

    (void)src;
    if (n >= 14 && buf[12] == 0x88 && buf[13] == 0x8e) {
        e = buf + 14;
        n -= 14;
    } else if (n >= 8 && buf[0] == 0xaa && buf[6] == 0x88) {
        e = buf + 8;
        n -= 8;
    }
    if (n < 4 || e[1] != 3)
        return 0;
    blen = be16(e + 2);
    if ((uint32_t)blen + 4 > n)
        blen = (uint16_t)(n - 4);
    if (blen < 95 || blen > 156)
        return 0;
    body = e + 4;
    ki = be16(body + 1);

    if (!(ki & KI_ACK) || !(ki & KI_PAIRWISE))
        return 0;

    if (!(ki & KI_MIC)) {
        /* Message 1/4: ANonce + replay counter. */
        if (!ws.have_pmk)
            return 0;
        memcpy(ws.anonce, body + 13, 32);
        memcpy(ws.replay, body + 5, 8);
        derive_ptk();
        send_msg2();
        ws.pending_ptk = 1;
        kprintf("[wpa] rx 1/4, sent 2/4\n");
        return 1;
    }

    /* Message 3/4: verify MIC, unwrap GTK, complete. */
    {
        uint8_t mic[20];
        uint16_t kdlen;
        uint8_t kd[160];
        int kdn = 0, i;

        ws.m3_ki = ki;
        memcpy(ws.replay, body + 5, 8);
        memcpy(copy, e, 4 + blen);
        memset(copy + 4 + 77, 0, 16);
        wifi_hmac_sha1(ws.ptk, 16, copy, 4 + blen, mic);
        if (memcmp(mic, body + 77, 16) != 0) {
            kprintf("[wpa] 3/4 MIC mismatch\n");
            return 0;
        }
        kdlen = be16(body + 93);
        if ((uint32_t)kdlen + 95 > blen)
            kdlen = (uint16_t)(blen - 95);
        if (ki & KI_ENCR) {
            kdn = aes_unwrap(ws.ptk + 16, body + 95, kdlen, kd);
            if (kdn < 0) {
                kprintf("[wpa] GTK unwrap failed\n");
                kdn = 0;
            }
        } else if (kdlen) {
            memcpy(kd, body + 95, kdlen);
            kdn = kdlen;
        }

        for (i = 0; i + 1 < kdn;) {
            uint8_t id = kd[i];
            uint8_t l = kd[i + 1];

            if (l == 0) {
                i += 2;
                continue;
            }
            if (i + 2 + l > kdn)
                break;
            if (id == 0xdd && l >= 6 && kd[i + 2] == 0x00 &&
                kd[i + 3] == 0x0f && kd[i + 4] == 0xac && kd[i + 5] == 1) {
                uint8_t keyid = (uint8_t)(kd[i + 6] & 3);

                kprintf("[wpa] GTK id=%u len=%u\n", keyid, (unsigned)(l - 6));
                esp_wifi_set_sta_key_internal(ALG_CCMP, ws.bssid, keyid, 0,
                                              NULL, 0, kd + i + 8, l - 6,
                                              KEY_FLAG_GROUP | KEY_FLAG_RX);
            }
            i += 2 + l;
        }

        send_msg4();
        ws.pending_activate = 1;
        kprintf("[wpa] rx 3/4, sent 4/4\n");
    }
    return 1;
}

static bool wpa_sta_in_4way_handshake(void)
{
    return ws.phase == 1;
}

int wifi_wpa_connected(void)
{
    return ws.phase == 2;
}

static int wpa_michael_mic_failure(uint16_t is_unicast)
{
    (void)is_unicast;
    return 0;
}

static void wpa_config_done(void)
{
}

static bool wpa_sta_profile_match(uint8_t *bssid)
{
    (void)bssid;
    return true;
}

static int wpa_sta_set_ap_rsnxe(const uint8_t *rsnxe, size_t len)
{
    (void)rsnxe; (void)len;
    return 0;
}

static struct wpa_funcs g_wpa = {
    .wpa_sta_init = wpa_sta_init,
    .wpa_sta_deinit = wpa_sta_deinit,
    .wpa_sta_connect = wpa_sta_connect,
    .wpa_sta_connected_cb = wpa_sta_connected_cb,
    .wpa_sta_disconnected_cb = wpa_sta_disconnected_cb,
    .wpa_sta_rx_eapol = wpa_sta_rx_eapol,
    .wpa_sta_in_4way_handshake = wpa_sta_in_4way_handshake,
    .wpa_parse_wpa_ie = tos_parse_wpa_ie,
    .wpa_michael_mic_failure = wpa_michael_mic_failure,
    .wpa_config_done = wpa_config_done,
    .wpa_sta_profile_match = wpa_sta_profile_match,
    .wpa_sta_set_ap_rsnxe = wpa_sta_set_ap_rsnxe,
};

void wifi_wpa_register(void)
{
    int rc = esp_wifi_register_wpa_cb_internal(&g_wpa);

    kprintf("[wpa] register rc=%d\n", rc);
}
