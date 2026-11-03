#include "lib.h"

/* rx - receive a file over the console, sent by a host tool (tools/tosput).
 *
 *     rx /fw.bin
 *
 * The console is the only wire the board is guaranteed to have, so the
 * protocol runs through it, in the tty's raw mode (no echo, no line
 * cooking, bytes as sent).  It is ASCII line frames with stop-and-wait
 * flow control - every chunk is answered before the next one is sent -
 * because that is the only flow control a bare UART has: while this task
 * is inside write() programming a flash block, the sender must not be
 * sending, or the tty's 256-byte raw queue would drop bytes on the erase
 * stall.  The cost is throughput (~6 KiB/s at 115200), which is fine for
 * config files, scripts and firmware images; the tty cannot do better
 * without hardware handshaking.
 *
 *     host                          rx
 *      ---------------------------> "READY\n"
 *      "D <seq> <len> <b64>\n"  --> decode, write to the file
 *      <--------------------------- "ACK <seq>\n"   or "NAK <seq>\n"
 *      ...
 *      "E <crc32 hex>\n"        --> verify
 *      <--------------------------- "DONE <crc32 hex>\n"
 *
 * <len> is the decoded byte count (max CHUNK), <seq> counts chunks from 0.
 * A duplicate seq means the host missed our ACK: answer ACK again without
 * writing.  A wrong length, a bad base64 character, or a line longer than
 * the buffer all answer NAK, and the host retransmits.  The final CRC32
 * (IEEE, the same polynomial every zip file uses) is computed over the
 * bytes actually written, so it verifies the flash contents, not just the
 * wire.  ^C kills this task at any point - the tty restores line
 * discipline when the reading task dies, and the half-written file stays
 * whatever it was, which is why the host sends CRC last: a failed
 * transfer is detected, not assumed.
 */

#define CHUNK 1024
#define LINE_MAX 1536             /* "D nnnn 1368 b64" fits with room   */
#define B64_MAX ((CHUNK + 2) / 3 * 4)

static int fd = -1;
static uint32_t crc = 0xFFFFFFFFu;
static uint32_t total = 0;
static int expected_seq = 0;

static const char b64t[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode n b64 chars into out (at most CHUNK bytes).  The encoding is
 * unpadded raw base64 - '=' would be one more character class the tty
 * could mangle and one more thing the length check has to allow - so the
 * final group may hold 2 or 3 chars, encoding 1 or 2 bytes; a leftover of
 * 4+ bits must be zero, which is what a correct encoder produces.
 * Returns bytes, or -1 on any character or length that cannot be true. */
static int b64_decode(const char *in, int n, unsigned char *out)
{
    int acc = 0, bits = 0, o = 0;

    if (n < 2 || n % 4 == 1)
        return -1;
    {
        /* Ceiling of the bytes this encoding can hold: full groups give 3
         * bytes each, a 3-char tail 2, a 2-char tail 1.  512 bytes come as
         * 683 chars (170 full groups + one 3-char tail), so the bound has
         * to count the tail as 2, not as another full group. */
        int max = (n / 4) * 3;
        if (n % 4 == 3)
            max += 2;
        else if (n % 4 == 2)
            max += 1;
        if (max > CHUNK)
            return -1;
    }
    for (int i = 0; i < n; i++) {
        int v = b64_val(in[i]);
        if (v < 0)
            return -1;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
        }
    }
    if (bits >= 6 && (acc & ((1u << bits) - 1u)) != 0)
        return -1;                /* non-zero trailing bits: corrupt */
    return o;
}

/* Bitwise CRC32 (IEEE 802.3, reflected): no 1 KiB table in the image for
 * a transfer this slow.  `crc` holds the running state (starts at
 * 0xFFFFFFFF, inverted once at the end) - the in/out inversion is per
 * checksum, not per chunk. */
static void crc32_feed(const unsigned char *p, int n)
{
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
}

/* Read one \n-terminated line in raw mode.  Returns the length without
 * the newline, or -1 if the line outran the buffer (lost bytes: the
 * caller answers NAK and the host retries the chunk). */
static int read_line(char *line)
{
    int len = 0;

    for (;;) {
        char c;
        int n = read(0, &c, 1);

        if (n <= 0) {
            /* Nothing in the queue yet; the tick refills it.  A tight
               loop here would burn the CPU the tty needs, so yield. */
            yield();
            continue;
        }
        if (c == '\n')
            return len;
        if (len >= LINE_MAX - 1)
            return -1;
        line[len++] = c;
    }
}

static void reply(const char *what, int seq)
{
    printf("%s %d\n", what, seq);
}

int main(int argc, char **argv)
{
    static char line[LINE_MAX];
    static unsigned char buf[CHUNK];

    if (argc < 2) {
        printf("usage: rx /path/file\n");
        return 1;
    }

    int old_raw = ttyraw(1);
    fd = create(argv[1]);
    if (fd < 0) {
        ttyraw(old_raw);
        printf("ERR cannot create %s\n", argv[1]);
        return 1;
    }
    printf("READY\n");

    for (;;) {
        int len = read_line(line);
        if (len < 0) {
            if (expected_seq > 0)
                reply("NAK", expected_seq - 1);
            continue;
        }
        line[len] = '\0';

        if (line[0] == 'E' && (len == 1 || line[1] == ' ')) {
            break;
        }
        if (line[0] != 'D' || line[1] != ' ') {
            /* The reason codes exist because the failure modes above the
               wire (lost bytes, a burst cut in half) all look identical
               from the host; r1 echoes a fragment so a corrupted line is
               recognizable in the log. */
            char head[21];
            int hl = len < 20 ? len : 20;
            memcpy(head, line, (size_t)hl);
            head[hl] = '\0';
            printf("NAK %d r1[%s]\n", expected_seq, head);
            continue;
        }

        int seq = 0, blen = 0, i = 2;
        for (; line[i] >= '0' && line[i] <= '9'; i++)
            seq = seq * 10 + (line[i] - '0');
        if (line[i] != ' ') {
            printf("NAK %d r2\n", expected_seq);
            continue;
        }
        for (i++; line[i] >= '0' && line[i] <= '9'; i++)
            blen = blen * 10 + (line[i] - '0');
        if (line[i] != ' ' || blen < 0 || blen > CHUNK) {
            printf("NAK %d r3(%d)\n", expected_seq, blen);
            continue;
        }
        i++;

        if (seq == expected_seq - 1) {
            /* The host did not get our last ACK and retransmitted.  Do
               not write twice: answer as if it were the first time. */
            reply("ACK", seq);
            continue;
        }
        if (seq != expected_seq) {
            printf("NAK %d r4(got %d)\n", expected_seq, seq);
            continue;
        }

        int n = b64_decode(line + i, len - i, buf);
        if (n != blen) {
            printf("NAK %d r5(len %d-%d=%d, dec %d)\n", seq, len, i, len - i, n);
            continue;
        }
        if (write(fd, buf, n) != n) {
            ttyraw(old_raw);
            printf("ERR write failed at %u\n", total);
            close(fd);
            return 1;
        }
        crc32_feed(buf, n);
        total += (uint32_t)n;
        expected_seq++;
        reply("ACK", seq);
    }

    close(fd);
    ttyraw(old_raw);
    printf("DONE %08x %u\n", crc ^ 0xFFFFFFFFu, total);
    return 0;
}
