/* bench - is a byte-at-a-time copy slow, and is it slow in a way that
 * depends on the *shape* of the loop rather than on the work done?
 *
 * This exists because QEMU's TCG can go pathological on rv32: it
 * re-translates the same tight loop for every iteration, which made the
 * kernel's memcpy 13x slower for the same byte count and the same number
 * of calls (docs/PLATFORM-DESIGN.md, "rv32 在 QEMU 下极慢").  Same guest
 * binary, same instructions, wildly different wall time.
 *
 * So the payload is deliberately two loops over the same number of bytes:
 *
 *   a1-byte    one byte per iteration, ~6 instructions per byte
 *   b-word     one word per iteration, ~4 instructions per word
 *   a2-byte    a1-byte again: same code, already executed once
 *
 * A healthy interpreter costs roughly (instructions executed) x (constant),
 * so a1 and a2 must match and b must be ~4-6x cheaper per byte.  If a2 is
 * much faster than a1, something cached the translation; if a1 costs
 * microseconds per *iteration*, something is re-translating.
 *
 * The host is expected to timestamp the START/END lines: `uptime()` is
 * wall time on QEMU but the instruction-count tick on RV32EMU, so it cannot
 * answer "how fast" on its own. */
#include "lib.h"

#define BYTES 4096              /* per buffer, 2 buffers = 8 KiB of heap */
#define REPS  64                /* 256 KiB copied per phase */

static int bytes = BYTES;       /* runtime size, so a machine that is */
static int reps = REPS;         /* absurdly slow can be measured small */

/* volatile on purpose: without it clang turns either loop into a memcpy()
 * call and the benchmark measures the wrong thing. */
static void copy_bytes(volatile unsigned char *d,
                       volatile const unsigned char *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static void copy_words(volatile unsigned int *d,
                       volatile const unsigned int *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static unsigned run_byte(const char *tag, unsigned char *d, unsigned char *s)
{
    int t0 = uptime();
    unsigned sum = 0;
    for (int r = 0; r < reps; r++)
        copy_bytes(d, s, bytes);
    t0 = uptime() - t0;
    sum = d[0] ^ d[bytes - 1];
    puts("END "); puts(tag); puts(" ticks="); putint(t0);
    puts(" sum="); putint((int)sum); puts("\n");
    return sum;
}

static unsigned run_word(const char *tag, unsigned char *d, unsigned char *s)
{
    int t0 = uptime();
    unsigned sum = 0;
    for (int r = 0; r < reps; r++)
        copy_words((volatile unsigned int *)d, (volatile const unsigned int *)s,
                   bytes / 4);
    t0 = uptime() - t0;
    sum = d[0] ^ d[bytes - 1];
    puts("END "); puts(tag); puts(" ticks="); putint(t0);
    puts(" sum="); putint((int)sum); puts("\n");
    return sum;
}

int main(int argc, char *argv[])
{
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v >= 4 && v <= BYTES)
            bytes = v & ~3;
    }
    if (argc > 2) {
        int v = atoi(argv[2]);
        if (v >= 1 && v <= 1000)
            reps = v;
    }

    unsigned char *a = malloc(bytes);
    unsigned char *b = malloc(bytes);
    if (!a || !b) {
        puts("bench: out of memory\n");
        return 1;
    }
    for (int i = 0; i < bytes; i++)
        a[i] = (unsigned char)(i * 7);

    puts("bench: "); putint(bytes); puts(" B x "); putint(reps);
    puts(" reps = "); putint(bytes * reps / 1024); puts(" KiB per phase\n");

    puts("START a1-byte\n");
    run_byte("a1-byte", b, a);

    puts("START b-word\n");
    run_word("b-word", b, a);

    puts("START a2-byte\n");
    run_byte("a2-byte", b, a);

    free(b);
    free(a);
    puts("done\n");
    return 0;
}
