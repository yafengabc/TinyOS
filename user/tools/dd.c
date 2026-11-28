#include "lib.h"

/* dd - block copy for the filesystem, in the spirit of the POSIX tool but
 * with only what an embedded test needs:
 *
 *   dd of=/big.bin bs=4096 count=100   write 100 blocks of a pattern
 *                                      (no if= means "make data")
 *   dd if=/big.bin bs=4096 count=100   read them back and verify (no of=
 *                                      means "read and check")
 *
 * The pattern is a linear congruential generator seeded the same way on
 * every run, so the read side can regenerate the expected bytes and compare
 * them block by block instead of trusting that "no error" means "same
 * data".  That is what makes the tool useful for filesystem testing: it can
 * fill the part of a partition beyond the shipped image (littlefs_fs_grow
 * territory) and prove the blocks read back.
 *
 * There is no seek syscall yet, so no skip=/seek=: everything is sequential
 * from the start of the file.  bs caps at 4096 (one littlefs block), but
 * the buffer is BUF_MAX and a block is written or verified in chunks:
 * flash, not the syscall, is what a block costs (measured on the C3: 1 MiB
 * written at bs=4096, 1024 and 512 all take ~9.3 s, and a read+verify
 * 0.635 s at 4096 vs 0.650 s at 512).  A 512-byte buffer keeps the
 * multi-call image one span class lower on both ISAs.
 *
 * One buffer, not two: the read side regenerates the expected byte as it
 * walks the block (the LCG step is the same on both sides).
 */

#define MAX_BS  4096
#define BUF_MAX 512

static uint8_t buf[BUF_MAX];

/* Fill dst with the pattern continuing from *state.  The state carries
 * across calls so consecutive blocks are all different. */
static void pattern_fill(uint8_t *dst, uint32_t *state, int len)
{
    for (int i = 0; i < len; i++) {
        *state = *state * 1664525u + 1013904223u;
        dst[i] = (uint8_t)(*state >> 24);
    }
}

int applet_dd(int argc, char **argv)
{
    const char *iff = 0, *off = 0;
    int bs = 512, count = 1;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "if=", 3))
            iff = argv[i] + 3;
        else if (!strncmp(argv[i], "of=", 3))
            off = argv[i] + 3;
        else if (!strncmp(argv[i], "bs=", 3))
            bs = atoi(argv[i] + 3);
        else if (!strncmp(argv[i], "count=", 6))
            count = atoi(argv[i] + 6);
        else {
            printf("dd: unknown arg %s\n", argv[i]);
            return 2;
        }
    }
    if (bs < 1)
        bs = 1;
    if (bs > MAX_BS) {
        printf("dd: bs=%d too large (max %d)\n", bs, MAX_BS);
        return 2;
    }

    uint32_t state = 0x1234abcd;

    if (off) {
        /* Write side: create the file and fill count blocks. */
        int fd = create(off);
        if (fd < 0) {
            printf("dd: cannot create %s\n", off);
            return 1;
        }
        for (int b = 0; b < count; b++) {
            int done = 0;

            while (done < bs) {
                int chunk = bs - done;
                if (chunk > BUF_MAX)
                    chunk = BUF_MAX;
                pattern_fill(buf, &state, chunk);
                int n = write(fd, buf, chunk);
                if (n != chunk) {
                    printf("dd: write error at block %d (%d)\n", b, n);
                    close(fd);
                    return 1;
                }
                done += chunk;
            }
        }
        close(fd);
        printf("dd: wrote %d bytes to %s\n", bs * count, off);
        return 0;
    }

    /* Read side: read count blocks and compare against the regenerated
     * pattern.  The generator restarts at the same seed, so the bytes must
     * match exactly; anything else is corruption. */
    int fd = iff ? open(iff) : -1;
    if (fd < 0) {
        printf("dd: cannot open %s\n", iff ? iff : "(no if=)");
        return 1;
    }
    state = 0x1234abcd;
    int bad = 0;

    for (int b = 0; b < count && !bad; b++) {
        int done = 0;

        while (done < bs) {
            int chunk = bs - done;
            if (chunk > BUF_MAX)
                chunk = BUF_MAX;
            int n = read(fd, buf, chunk);
            if (n <= 0) {
                printf("dd: short read at block %d (%d)\n", b, n);
                close(fd);
                return 1;
            }
            for (int i = 0; i < n; i++) {
                state = state * 1664525u + 1013904223u;
                if (buf[i] != (uint8_t)(state >> 24)) {
                    printf("dd: data mismatch at block %d byte %d\n",
                           b, done + i);
                    bad = 1;
                    break;
                }
            }
            if (bad)
                break;
            done += n;
        }
    }
    close(fd);
    if (!bad)
        printf("dd: read %d bytes from %s, pattern verified\n",
               bs * count, iff);
    return bad ? 1 : 0;
}
