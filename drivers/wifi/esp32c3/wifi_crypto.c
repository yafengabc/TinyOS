/* WPA2 crypto callbacks for the Espressif WiFi blobs, built on the C3's
 * ROM SHA-1/SHA-256 and AES accelerators.  Scan does not need any of this;
 * association and the 4-way handshake do.
 */
#include "types.h"
#include "string.h"
#include "console.h"

#define AES128 0

typedef struct {
    uint32_t opaque[54];        /* 216 bytes: SHA_CTX */
} sha_ctx_t;

extern int  ets_sha_init(void *ctx, int type);
extern void ets_sha_starts(void *ctx, unsigned short sha512_t);
extern void ets_sha_update(void *ctx, const void *data, uint32_t len, int update_ctx);
extern int  ets_sha_finish(void *ctx, uint8_t *digest);
extern void ets_sha_enable(void);
extern void ets_sha_disable(void);

extern void ets_aes_enable(void);
extern void ets_aes_disable(void);
extern int  ets_aes_setkey_enc(const void *key, int bits);
extern int  ets_aes_setkey_dec(const void *key, int bits);
extern void ets_aes_block(const void *input, void *output);

#define SHA1_TYPE 0
#define SHA256_TYPE 2

static sha_ctx_t sha_ctx;

static void sha_begin(int type)
{
    ets_sha_enable();
    ets_sha_init(&sha_ctx, type);
    ets_sha_starts(&sha_ctx, 0);
}

static void sha_feed(const void *data, uint32_t len)
{
    if (len)
        ets_sha_update(&sha_ctx, data, len, 0);
}

static void sha_end(uint8_t *out)
{
    ets_sha_finish(&sha_ctx, out);
    ets_sha_disable();
}

static void sha1_buf(const void *data, uint32_t len, uint8_t out[20])
{
    sha_begin(SHA1_TYPE);
    sha_feed(data, len);
    sha_end(out);
}

static void sha256_buf(const void *data, uint32_t len, uint8_t out[32])
{
    sha_begin(SHA256_TYPE);
    sha_feed(data, len);
    sha_end(out);
}

/* ---- HMAC ------------------------------------------------------------ */

static void hmac_sha1(const uint8_t *key, uint32_t key_len,
                      const uint8_t *data, uint32_t data_len, uint8_t out[20])
{
    uint8_t k[64], ipad[64], opad[64], inner[20];

    memset(k, 0, sizeof(k));
    if (key_len > 64)
        sha1_buf(key, key_len, k);
    else
        memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha_begin(SHA1_TYPE);
    sha_feed(ipad, 64);
    sha_feed(data, data_len);
    sha_end(inner);
    sha_begin(SHA1_TYPE);
    sha_feed(opad, 64);
    sha_feed(inner, 20);
    sha_end(out);
}

static void hmac_sha256(const uint8_t *key, uint32_t key_len,
                        const uint8_t *data, uint32_t data_len, uint8_t out[32])
{
    uint8_t k[64], ipad[64], opad[64], inner[32];

    memset(k, 0, sizeof(k));
    if (key_len > 64)
        sha256_buf(key, key_len, k);
    else
        memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha_begin(SHA256_TYPE);
    sha_feed(ipad, 64);
    sha_feed(data, data_len);
    sha_end(inner);
    sha_begin(SHA256_TYPE);
    sha_feed(opad, 64);
    sha_feed(inner, 32);
    sha_end(out);
}

/* ---- AES ------------------------------------------------------------- */

static void aes_block_enc(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    ets_aes_enable();
    ets_aes_setkey_enc(key, AES128);
    ets_aes_block(in, out);
    ets_aes_disable();
}

static void aes_block_dec(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    ets_aes_enable();
    ets_aes_setkey_dec(key, AES128);
    ets_aes_block(in, out);
    ets_aes_disable();
}

static int aes_cbc_encrypt(const uint8_t *key, const uint8_t *iv,
                           uint8_t *data, int len)
{
    uint8_t prev[16], x[16];

    if (!len || (len & 15))
        return -1;
    memcpy(prev, iv, 16);
    for (int off = 0; off < len; off += 16) {
        for (int i = 0; i < 16; i++)
            x[i] = data[off + i] ^ prev[i];
        aes_block_enc(key, x, data + off);
        memcpy(prev, data + off, 16);
    }
    return 0;
}

static int aes_cbc_decrypt(const uint8_t *key, const uint8_t *iv,
                           uint8_t *data, int len)
{
    uint8_t prev[16], ct[16], x[16];

    if (!len || (len & 15))
        return -1;
    memcpy(prev, iv, 16);
    for (int off = 0; off < len; off += 16) {
        memcpy(ct, data + off, 16);
        aes_block_dec(key, ct, x);
        for (int i = 0; i < 16; i++)
            data[off + i] = x[i] ^ prev[i];
        memcpy(prev, ct, 16);
    }
    return 0;
}

static void gf_dbl(uint8_t *b)
{
    int carry = b[0] >> 7;

    for (int i = 0; i < 15; i++)
        b[i] = (uint8_t)((b[i] << 1) | (b[i + 1] >> 7));
    b[15] = (uint8_t)(b[15] << 1);
    if (carry)
        b[15] ^= 0x87;
}

/* AES-CMAC (OMAC1) over one buffer. */
static int omac1(const uint8_t *key, const uint8_t *data, size_t len,
                 uint8_t mic[16])
{
    uint8_t l[16], k1[16], k2[16], x[16], last[16];
    size_t blocks = (len + 15) / 16;
    int complete = len && (len % 16 == 0);

    memset(l, 0, 16);
    aes_block_enc(key, l, l);
    memcpy(k1, l, 16);
    gf_dbl(k1);
    memcpy(k2, k1, 16);
    gf_dbl(k2);

    memset(x, 0, 16);
    for (size_t b = 0; b + 1 < blocks; b++) {
        for (int i = 0; i < 16; i++)
            x[i] ^= data[b * 16 + i];
        aes_block_enc(key, x, x);
    }
    if (complete)
        memcpy(last, data + (blocks - 1) * 16, 16);
    else {
        size_t rem = len - (blocks - 1) * 16;

        memset(last, 0, 16);
        if (rem)
            memcpy(last, data + (blocks - 1) * 16, rem);
        last[rem] = 0x80;
    }
    {
        const uint8_t *k = complete ? k1 : k2;

        for (int i = 0; i < 16; i++)
            last[i] ^= k[i];
    }
    for (int i = 0; i < 16; i++)
        x[i] ^= last[i];
    aes_block_enc(key, x, mic);
    return 0;
}

/* ---- callbacks ------------------------------------------------------- */

static int cb_hmac_sha256_vector(const uint8_t *key, int key_len, int num_elem,
                                 const uint8_t *addr[], const int *len,
                                 uint8_t *mac)
{
    uint8_t k[64], ipad[64], opad[64], inner[32];

    memset(k, 0, sizeof(k));
    if (key_len > 64)
        sha256_buf(key, key_len, k);
    else
        memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha_begin(SHA256_TYPE);
    sha_feed(ipad, 64);
    for (int e = 0; e < num_elem; e++)
        sha_feed(addr[e], (uint32_t)len[e]);
    sha_end(inner);
    sha_begin(SHA256_TYPE);
    sha_feed(opad, 64);
    sha_feed(inner, 32);
    sha_end(mac);
    return 0;
}

static int cb_pbkdf2_sha1(const char *passphrase, const char *ssid,
                          unsigned int ssid_len, int iterations,
                          unsigned char *buf, unsigned int buflen)
{
    uint8_t u[20], t[20], salt[64];
    uint32_t pass_len = strlen(passphrase);

    for (uint32_t block = 1, done = 0; done < buflen; block++, done += 20) {
        if (ssid_len > sizeof(salt) - 4)
            return -1;
        memcpy(salt, ssid, ssid_len);
        salt[ssid_len + 0] = (uint8_t)(block >> 24);
        salt[ssid_len + 1] = (uint8_t)(block >> 16);
        salt[ssid_len + 2] = (uint8_t)(block >> 8);
        salt[ssid_len + 3] = (uint8_t)block;

        hmac_sha1((const uint8_t *)passphrase, pass_len,
                  salt, ssid_len + 4, u);
        memcpy(t, u, 20);
        for (int i = 1; i < iterations; i++) {
            hmac_sha1((const uint8_t *)passphrase, pass_len, u, 20, u);
            for (int j = 0; j < 20; j++)
                t[j] ^= u[j];
        }
        for (uint32_t i = 0; i < 20 && done + i < buflen; i++)
            buf[done + i] = t[i];
    }
    return 0;
}

static int cb_aes_128_encrypt(const unsigned char *key, const unsigned char *iv,
                              unsigned char *data, int data_len)
{
    return aes_cbc_encrypt(key, iv, data, data_len);
}

static int cb_aes_128_decrypt(const unsigned char *key, const unsigned char *iv,
                              unsigned char *data, int data_len)
{
    return aes_cbc_decrypt(key, iv, data, data_len);
}

static int cb_omac1_aes_128(const uint8_t *key, const uint8_t *data,
                            size_t data_len, uint8_t *mic)
{
    return omac1(key, data, data_len, mic);
}

static int cb_sha256_vector(size_t num_elem, const uint8_t *addr[],
                            const size_t *len, uint8_t *buf)
{
    sha_begin(SHA256_TYPE);
    for (size_t e = 0; e < num_elem; e++)
        sha_feed(addr[e], (uint32_t)len[e]);
    sha_end(buf);
    return 0;
}

static uint8_t *cb_ccmp_decrypt(const uint8_t *tk, const uint8_t *hdr,
                                const uint8_t *data, size_t data_len,
                                size_t *decrypted_len, int espnow)
{
    (void)tk; (void)hdr; (void)data; (void)data_len; (void)espnow;
    if (decrypted_len)
        *decrypted_len = 0;
    return 0;
}

static uint8_t *cb_ccmp_encrypt(const uint8_t *tk, uint8_t *frame, size_t len,
                                size_t hdrlen, uint8_t *pn, int keyid,
                                size_t *encrypted_len)
{
    (void)tk; (void)frame; (void)len; (void)hdrlen; (void)pn; (void)keyid;
    if (encrypted_len)
        *encrypted_len = 0;
    return 0;
}

static int cb_aes_gmac(const uint8_t *key, size_t keylen, const uint8_t *iv,
                       size_t iv_len, const uint8_t *aad, size_t aad_len,
                       uint8_t *mic)
{
    (void)key; (void)keylen; (void)iv; (void)iv_len; (void)aad; (void)aad_len; (void)mic;
    return -1;
}

void wifi_crypto_fill(uint32_t *raw)
{
    raw[0] = 44;                /* sizeof(wpa_crypto_funcs_t) */
    raw[1] = 1;                 /* ESP_WIFI_CRYPTO_VERSION */
    raw[2] = (uint32_t)cb_hmac_sha256_vector;
    raw[3] = (uint32_t)cb_pbkdf2_sha1;
    raw[4] = (uint32_t)cb_aes_128_encrypt;
    raw[5] = (uint32_t)cb_aes_128_decrypt;
    raw[6] = (uint32_t)cb_omac1_aes_128;
    raw[7] = (uint32_t)cb_ccmp_decrypt;
    raw[8] = (uint32_t)cb_ccmp_encrypt;
    raw[9] = (uint32_t)cb_aes_gmac;
    raw[10] = (uint32_t)cb_sha256_vector;
}

/* Known-answer test so a ROM-call mistake shows up at boot, not inside the
 * 4-way handshake. */
void wifi_crypto_selftest(void)
{
    static const uint8_t abc[3] = {'a', 'b', 'c'};
    uint8_t md[32];
    const uint8_t want1[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };
    const uint8_t want256[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };

    sha1_buf(abc, 3, md);
    int ok1 = memcmp(md, want1, 20) == 0;
    sha256_buf(abc, 3, md);
    int ok256 = memcmp(md, want256, 32) == 0;
    {
        static const uint8_t hkey[20] = {
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b
        };
        static const uint8_t want_hmac[20] = {
            0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
            0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00
        };
        uint8_t pmk[32];
        static const uint8_t want_pmk[32] = {
            0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,
            0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
            0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,
            0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e
        };
        int ok_hmac, ok_pmk;

        hmac_sha1(hkey, 20, (const uint8_t *)"Hi There", 8, md);
        ok_hmac = memcmp(md, want_hmac, 20) == 0;
        cb_pbkdf2_sha1("password", "IEEE", 4, 4096, pmk, 32);
        ok_pmk = memcmp(pmk, want_pmk, 32) == 0;
        kprintf("[wifi] crypto hmac=%s pbkdf2=%s\n",
                ok_hmac ? "ok" : "BAD", ok_pmk ? "ok" : "BAD");
    }
    kprintf("[wifi] crypto sha1=%s sha256=%s\n",
            ok1 ? "ok" : "BAD", ok256 ? "ok" : "BAD");
}

/* ---- helpers for the minimal WPA2 supplicant ------------------------- */

void wifi_hmac_sha1(const uint8_t *key, uint32_t key_len,
                    const uint8_t *data, uint32_t data_len, uint8_t out[20])
{
    hmac_sha1(key, key_len, data, data_len, out);
}

void wifi_pbkdf2_sha1(const char *pass, const char *ssid, unsigned ssid_len,
                      int iterations, uint8_t *buf, unsigned buflen)
{
    cb_pbkdf2_sha1(pass, ssid, ssid_len, iterations, buf, buflen);
}

void wifi_aes_block_enc(const uint8_t key[16], const uint8_t in[16],
                        uint8_t out[16])
{
    aes_block_enc(key, in, out);
}

void wifi_aes_block_dec(const uint8_t key[16], const uint8_t in[16],
                        uint8_t out[16])
{
    aes_block_dec(key, in, out);
}
