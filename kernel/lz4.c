#include "lz4.h"
#include "string.h"

/* LZ4 block format reference decoder.

   A block is a sequence of:
     token (1 byte)  high nibble = literal length, low nibble = match length
     [extra literal-length bytes, each 255 means "add 255 and continue"]
     literals
     offset (2 bytes, little endian, 1..65535)
     [extra match-length bytes, 4 is added to the 4 minimum]

   We do bounds checks on both sides; a corrupt block returns -1 instead of
   reading past the source or writing past the destination. */

static int read_len(const uint8_t *src, uint32_t src_len, uint32_t *pos, uint32_t base)
{
    uint32_t n = base;
    for (;;) {
        if (*pos >= src_len)
            return -1;
        uint8_t b = src[(*pos)++];
        n += b;
        if (b != 255)
            break;
    }
    return (int)n;
}

int lz4_decompress_block(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap)
{
    uint32_t ip = 0;
    uint32_t op = 0;

    while (ip < src_len) {
        uint8_t token = src[ip++];

        /* literals */
        uint32_t lit = token >> 4;
        if (lit == 15) {
            int extra = read_len(src, src_len, &ip, 0);
            if (extra < 0)
                return -1;
            lit += (uint32_t)extra;
        }
        if (ip + lit > src_len || op + lit > dst_cap)
            return -1;
        memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;

        if (ip == src_len)
            break;                       /* last sequence has no match */

        /* match */
        if (ip + 2 > src_len)
            return -1;
        uint32_t offset = (uint32_t)src[ip] | ((uint32_t)src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op)
            return -1;

        uint32_t mlen = token & 0x0F;
        if (mlen == 15) {
            int extra = read_len(src, src_len, &ip, 0);
            if (extra < 0)
                return -1;
            mlen += (uint32_t)extra;
        }
        mlen += 4;                       /* minimum match */

        if (op + mlen > dst_cap)
            return -1;

        /* byte-wise copy: matches may overlap the output */
        uint32_t from = op - offset;
        for (uint32_t i = 0; i < mlen; i++)
            dst[op + i] = dst[from + i];
        op += mlen;
    }
    return (int)op;
}

static uint32_t wrap_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t lz4_wrap_info(const uint8_t *data, uint32_t size,
                       uint32_t *out_size,
                       lz4_window_hint_t *hint, int *has_hint)
{
    if (size < LZ4_WRAP_HEADER)
        return 0;
    if (memcmp(data, LZ4_WRAP_MAGIC, 6) != 0)
        return 0;

    /* An unknown flag bit may change where the block starts; refuse the
       envelope rather than decode from a guessed offset. */
    uint32_t flags = (uint32_t)data[6] | ((uint32_t)data[7] << 8);
    uint32_t hdr = LZ4_WRAP_HEADER;
    if (flags & LZ4_WRAP_HINT_BIT) {
        if (flags != LZ4_WRAP_HINT_BIT)
            return 0;
        hdr = LZ4_WRAP_HEADER_V1;
    } else if (flags != 0) {
        return 0;
    }
    if (size < hdr)
        return 0;

    uint32_t orig = wrap_le32(data + 8);
    uint32_t comp = wrap_le32(data + 12);
    if (comp != size - hdr)
        return 0;

    if (out_size)
        *out_size = orig;
    if (has_hint)
        *has_hint = (flags == LZ4_WRAP_HINT_BIT);
    if (hint && flags == LZ4_WRAP_HINT_BIT) {
        hint->span      = wrap_le32(data + 16);
        hint->stack_ask = wrap_le32(data + 20);
        hint->heap_ask  = wrap_le32(data + 24);
    }
    return hdr;
}
