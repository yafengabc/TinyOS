#include "lz4.h"
#include "string.h"

/* LZ4 block format:
     token (1 byte)  high nibble = literal length, low nibble = match length
     [extra literal-length bytes, each 255 means "add 255 and continue"]
     literals
     offset (2 bytes, little endian, 1..65535)
     [extra match-length bytes, 4 is added to the 4 minimum]

   The streaming decoder bounds both sides; a corrupt block returns -1
   instead of reading past the source or writing past the destination. */

static uint32_t wrap_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t lz4_wrap_info_prefix(const uint8_t *prefix, uint32_t prefix_len,
                              uint32_t size, uint32_t *out_size,
                              lz4_window_hint_t *hint)
{
    /* Only one envelope shape exists: exec() must know the window size
       before it decompresses, so a header without the hint is not loadable. */
    if (size < LZ4_WRAP_HEADER_V1 || prefix_len < LZ4_WRAP_HEADER_V1)
        return 0;
    if (memcmp(prefix, LZ4_WRAP_MAGIC, 6) != 0)
        return 0;
    uint32_t flags = (uint32_t)prefix[6] | ((uint32_t)prefix[7] << 8);
    if (flags != LZ4_WRAP_HINT_BIT)
        return 0;

    uint32_t orig = wrap_le32(prefix + 8);
    uint32_t comp = wrap_le32(prefix + 12);
    if (orig == 0 || comp != size - LZ4_WRAP_HEADER_V1)
        return 0;

    if (out_size)
        *out_size = orig;
    if (hint) {
        hint->span      = wrap_le32(prefix + 16);
        hint->stack_ask = wrap_le32(prefix + 20);
        hint->heap_ask  = wrap_le32(prefix + 24);
    }
    return LZ4_WRAP_HEADER_V1;
}

/* ---- streaming decoder ------------------------------------------------ */

/* Keep whatever is left of the current buffer, then ask the reader for more.
   The callback is allowed to short-read; it is expected to fill `cap` unless
   it has reached EOF.  A single refill is enough because get_byte() consumes
   one byte at a time - there is no need to accumulate a whole token. */
static int stream_refill(lz4_stream_t *s)
{
    if (s->eof)
        return 0;

    uint32_t keep = s->ilen - s->ip;
    if (keep)
        memmove(s->in, s->in + s->ip, keep);
    s->ip = 0;
    s->ilen = keep;

    if (s->ilen == sizeof(s->in))
        return 0;                  /* buffer already full */

    int n = s->read(s->ctx, s->in + s->ilen,
                    (uint32_t)sizeof(s->in) - s->ilen);
    if (n < 0)
        return -1;
    if (n == 0)
        s->eof = 1;
    else
        s->ilen += (uint32_t)n;
    return 0;
}

/* 1 = got a byte, 0 = clean EOF, -1 = read error. */
static int stream_get_byte(lz4_stream_t *s, uint8_t *out)
{
    if (s->ip >= s->ilen) {
        if (stream_refill(s) < 0)
            return -1;
        if (s->ip >= s->ilen)
            return 0;
    }
    *out = s->in[s->ip++];
    return 1;
}

/* Read a 255-terminated length extension across refill boundaries.
   `acc` is the nibble value already seen. */
static int stream_read_len(lz4_stream_t *s, uint32_t *acc)
{
    uint32_t n = *acc;
    for (;;) {
        uint8_t b;
        int r = stream_get_byte(s, &b);
        if (r <= 0)
            return -1;             /* EOF inside a length run is corrupt */
        n += b;
        if (b != 255)
            break;
        if (n > 0xFFFFFFu)
            return -1;
    }
    *acc = n;
    return 0;
}

int lz4_stream_decompress(lz4_stream_t *s, uint8_t *dst, uint32_t dst_cap)
{
    uint32_t op = 0;

    for (;;) {
        uint8_t token;
        int r = stream_get_byte(s, &token);
        if (r < 0)
            return -1;
        if (r == 0)
            break;                 /* clean end of block */

        uint32_t lit = token >> 4;
        if (lit == 15 && stream_read_len(s, &lit) != 0)
            return -1;
        if (lit > dst_cap - op)
            return -1;

        /* Literals may cross any number of refill boundaries.  Copying them
           one byte at a time keeps the overlap rules identical to the
           reference decoder; a batch copy would need a peekable buffer. */
        for (uint32_t i = 0; i < lit; i++) {
            uint8_t b;
            if (stream_get_byte(s, &b) <= 0)
                return -1;
            dst[op++] = b;
        }

        /* The last sequence has literals but no match.  Peek by consuming
           the first offset byte: EOF here is legal, EOF halfway through the
           two-byte offset is not. */
        uint8_t lo;
        r = stream_get_byte(s, &lo);
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        uint8_t hi;
        if (stream_get_byte(s, &hi) <= 0)
            return -1;

        uint32_t offset = (uint32_t)lo | ((uint32_t)hi << 8);
        if (offset == 0 || offset > op)
            return -1;

        uint32_t mlen = token & 0x0F;
        if (mlen == 15 && stream_read_len(s, &mlen) != 0)
            return -1;
        mlen += 4;
        if (mlen > dst_cap - op)
            return -1;

        /* Byte-wise so overlapping matches work.  `dst` is the linear
           output buffer, hence the output history is always readable. */
        uint32_t from = op - offset;
        for (uint32_t i = 0; i < mlen; i++)
            dst[op + i] = dst[from + i];
        op += mlen;
    }
    return (int)op;
}
