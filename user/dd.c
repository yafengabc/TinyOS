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
 * from the start of the file.  bs caps at 4096, one littlefs block, which
 * keeps the buffers out of the way of the 64 KiB task windows.
 */

#define MAX_BS 4096

static uint8_t buf[MAX_BS];
static uint8_t want[MAX_BS];

/* Fill dst with the pattern continuing from *state; returns the block's
 * 32-bit checksum.  The state carries across calls so consecutive blocks
 * are all different. */
static uint32_t pattern_fill(uint8_t *dst, uint32_t *state, int len)
{
    uint32_t sum = 0;

    for (int i = 0; i < len; i++) {
        *state = *state * 1664525u + 1013904223u;
        dst[i] = (uint8_t)(*state >> 24);
        sum = sum * 31u + dst[i];
    }
    return sum;
}

int main(int argc, char **argv)
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
            pattern_fill(buf, &state, bs);
            int n = write(fd, buf, bs);
            if (n != bs) {
                printf("dd: write error at block %d (%d)\n", b, n);
                close(fd);
                return 1;
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
        int n = read(fd, buf, bs);
        if (n <= 0) {
            printf("dd: short read at block %d (%d)\n", b, n);
            close(fd);
            return 1;
        }
        pattern_fill(want, &state, n);
        for (int i = 0; i < n; i++)
            if (buf[i] != want[i]) {
                printf("dd: data mismatch at block %d byte %d\n", b, i);
                bad = 1;
                break;
            }
    }
    close(fd);
    if (!bad)
        printf("dd: read %d bytes from %s, pattern verified\n",
               bs * count, iff);
    return bad ? 1 : 0;
}
