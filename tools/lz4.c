/*
 * lz4.c - minimal LZ4 block compressor (host side, used by mklfs)
 *
 * This is a from-scratch implementation of the LZ4 block format, not the
 * reference encoder.  It exists so the build tool can compress user ELFs
 * without pulling in an extra dependency; the kernel only needs a decoder.
 *
 * Format recap (see kernel/lz4.c for the decoder):
 *   token: high nibble literal length, low nibble match length-4
 *   literal length >= 15 -> extra bytes, each 255 continues
 *   match length   >= 19 -> extra bytes, each 255 continues
 *   offset: 2 bytes little endian, 1..65535
 *
 * The last 5 bytes must be literals and the last match must start at
 * least 12 bytes before the end of the block, which we satisfy by simply
 * not emitting matches near the tail.
 */
#include <stdint.h>
#include <string.h>

#define MIN_MATCH 4
#define MAX_OFFSET 65535
#define LAST_LITERALS 5
#define MF_LIMIT 12          /* matches must start this far from the end */

static int emit_len(uint8_t *dst, int op, int n)
{
    while (n >= 255) {
        dst[op++] = 255;
        n -= 255;
    }
    dst[op++] = (uint8_t)n;
    return op;
}

int lz4_compress_bound(int n)
{
    return n + n / 255 + 16;
}

/* Hash chain: for each 4-byte prefix keep the most recent position plus a
   link to the previous one, so we can try several candidates when the
   newest match fails or is too short.  Much better ratio than a single
   hash slot for the small, repetitive code in our user programs. */
#define HASH_BITS 14
#define HASH_SIZE (1 << HASH_BITS)
#define MAX_CHAIN 32

static uint32_t lz4_hash(const uint8_t *p)
{
    uint32_t seq;
    memcpy(&seq, p, 4);
    return (seq * 2654435761u) >> (32 - HASH_BITS);
}

int lz4_compress_block(const uint8_t *src, int src_len, uint8_t *dst)
{
    if (src_len <= 0)
        return 0;

    /* Static tables avoid 200 KB of stack; the build tool is single
       threaded and processes one file at a time. */
    static int32_t head[HASH_SIZE];
    static int32_t chain[1 << 20];

    memset(head, 0xFF, sizeof(head));

    const int last_match_start = src_len - MF_LIMIT;
    int anchor = 0;
    int ip = 0;
    int op = 0;

    while (ip <= last_match_start) {
        uint32_t h = lz4_hash(src + ip);
        int32_t cand = head[h];
        int best_len = 0;
        int best_off = 0;

        int probes = 0;
        int32_t c = cand;
        while (c >= 0 && probes++ < MAX_CHAIN) {
            int off = ip - c;
            if (off > MAX_OFFSET)
                break;
            int max_len = src_len - LAST_LITERALS - ip;
            if (src[c + best_len] == src[ip + best_len] &&
                memcmp(src + c, src + ip, MIN_MATCH) == 0) {
                int ml = MIN_MATCH;
                while (ml < max_len && src[c + ml] == src[ip + ml])
                    ml++;
                if (ml > best_len) {
                    best_len = ml;
                    best_off = off;
                    if (ml >= 64)
                        break;      /* good enough, stop probing */
                }
            }
            c = chain[c];
        }

        /* insert current position into the chain */
        chain[ip] = cand;
        head[h] = ip;

        if (best_len < MIN_MATCH) {
            ip++;
            continue;
        }

        /* literal run */
        int lit = ip - anchor;
        int token_pos = op++;
        if (lit >= 15) {
            dst[token_pos] = 0xF0;
            op = emit_len(dst, op, lit - 15);
        } else {
            dst[token_pos] = (uint8_t)(lit << 4);
        }
        memcpy(dst + op, src + anchor, lit);
        op += lit;

        dst[op++] = (uint8_t)(best_off & 0xFF);
        dst[op++] = (uint8_t)(best_off >> 8);

        int mlen = best_len - MIN_MATCH;
        dst[token_pos] |= (uint8_t)(mlen < 15 ? mlen : 15);
        if (mlen >= 15)
            op = emit_len(dst, op, mlen - 15);

        /* insert the positions covered by the match (lightly: only the
           start and a few follow-ups, to keep it O(n)) */
        for (int k = 1; k < best_len && ip + k <= last_match_start; k++) {
            uint32_t hk = lz4_hash(src + ip + k);
            chain[ip + k] = head[hk];
            head[hk] = ip + k;
        }

        ip += best_len;
        anchor = ip;
    }

    /* trailing literals */
    int lit = src_len - anchor;
    int token_pos = op++;
    if (lit >= 15) {
        dst[token_pos] = 0xF0;
        op = emit_len(dst, op, lit - 15);
    } else {
        dst[token_pos] = (uint8_t)(lit << 4);
    }
    memcpy(dst + op, src + anchor, lit);
    op += lit;

    return op;
}
