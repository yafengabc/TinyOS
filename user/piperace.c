#include "lib.h"

/* piperace - push bytes through a pipe with the reader on one CPU and the
 * writer on the other, and check that every byte arrives unchanged.
 *
 *   piperace                one pair, 64 KiB, one summary line
 *   piperace <pairs> <KiB>  the same, repeated
 *   piperace -w <KiB>       writer (what `piperace` spawns)
 *   piperace -r             reader and verifier
 *
 * The point is not throughput, it is how often each side blocks.  A 2 KiB
 * ring and 128-byte moves mean the writer fills it, parks, is woken, fills
 * again - and the reader drains it, parks, is woken - thousands of times per
 * run.  Every one of those is a "check the ring, then go to sleep" step, and
 * on two CPUs a waker can slip into the gap.  The visible symptom is a pipe
 * that stops moving forever; a ring that is written from both sides at once
 * shows up as a wrong byte.
 *
 * The pattern is a function of the byte's position, so the reader can check
 * every byte without trusting the writer's bookkeeping.  Exit codes are KiB
 * moved (the writer's is what it managed to send, which is what makes a
 * short run visible as a smaller number rather than a hang). */

#define BLK 128

static unsigned char pat(unsigned int off)
{
    return (unsigned char)(off * 37u + 11u);
}

/* exec() takes the children's arguments as strings, and lib has no itoa
 * (nothing else needs one), so the KiB count is formatted here. */
static void dec(char *buf, int v)
{
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && i < (int)sizeof(tmp));
    for (int k = 0; k < i; k++)
        buf[k] = tmp[i - 1 - k];
    buf[i] = '\0';
}

static int writer(int kib)
{
    unsigned char buf[BLK];
    unsigned int total = (unsigned int)kib * 1024u;
    unsigned int off = 0;

    while (off < total) {
        unsigned int n = total - off;

        if (n > BLK)
            n = BLK;
        for (unsigned int i = 0; i < n; i++)
            buf[i] = pat(off + i);
        if (write_all(1, buf, (int)n) != (int)n)
            return (int)(off / 1024u);
        off += n;
    }
    return kib;
}

static int reader(void)
{
    unsigned char buf[BLK];
    unsigned int off = 0;
    int n;
    int bad = 0;

    while ((n = read(0, buf, (int)sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++)
            if (buf[i] != pat(off + (unsigned int)i) && !bad) {
                bad = 1;
                printf("piperace: bad byte at %u: got %d want %d\n",
                       off + (unsigned int)i, (int)buf[i],
                       (int)pat(off + (unsigned int)i));
            }
        off += (unsigned int)n;
    }
    if (bad)
        return -1;
    return (int)(off / 1024u);
}

int main(int argc, char *argv[])
{
    int pairs = 1, kib = 64;
    int wpid[8], rpid[8];
    int is_child = argc >= 2 && argv[1][0] == '-';

    /* A child inherits the parent's whole descriptor table, so this one
     * arrived holding both ends of its own pipe (the shell's pipeline has
     * the same property and never notices).  Drop the extra copies before
     * the first transfer: then a partner that never started - exec can fail
     * when the window allocator is full - shows up as EPIPE on this end
     * instead of the writer parking forever on a pipe whose only remaining
     * reader is itself.  Descriptors 0..2 are what exec() wired up. */
    if (is_child)
        for (int fd = 3; fd < 12; fd++)
            close(fd);

    if (argc >= 2 && !strcmp(argv[1], "-w"))
        return writer(argc >= 3 ? atoi(argv[2]) : 64);
    if (argc >= 2 && !strcmp(argv[1], "-r"))
        return reader();

    if (argc >= 2)
        pairs = atoi(argv[1]);
    if (argc >= 3)
        kib = atoi(argv[2]);
    if (pairs < 1 || pairs > (int)(sizeof(wpid) / sizeof(wpid[0])))
        pairs = 1;
    if (kib < 1)
        kib = 1;

    for (int i = 0; i < pairs; i++) {
        char kb[8], *wargs[3];
        int fds[2];

        wpid[i] = rpid[i] = -1;
        if (pipe(fds) < 0) {
            printf("piperace: no descriptors for pair %d\n", i);
            pairs = i;
            break;
        }

        /* Wire the writer first, then drop every write end the parent holds
         * before the reader starts: EOF means "no write end left anywhere",
         * and the parent's own copy would keep the reader waiting forever. */
        if (dup2(fds[1], 1) < 0) {
            close(fds[0]);
            close(fds[1]);
            pairs = i;
            break;
        }
        dec(kb, kib);
        wargs[0] = "piperace";
        wargs[1] = "-w";
        wargs[2] = kb;
        wpid[i] = exec("bin/piperace", wargs, 3);
        close(1);                   /* back to the console on our side */
        close(fds[1]);

        if (dup2(fds[0], 0) < 0) {
            close(fds[0]);
            pairs = i;
            break;
        }
        char *rargs[2] = { "piperace", "-r" };
        rpid[i] = exec("bin/piperace", rargs, 2);
        close(0);
        close(fds[0]);
        printf("  spawn p%d: w=%d r=%d\n", i, wpid[i], rpid[i]);
    }

    /* One line, from the parent: the console drops lines when several tasks
     * finish together, so per-pair reporting would lose exactly the pair
     * that went wrong. */
    int bad = 0;
    printf("piperace: %d pair(s), %d KiB each\n", pairs, kib);
    for (int i = 0; i < pairs; i++) {
        int w = wpid[i] >= 0 ? wait(wpid[i]) : -1;
        int r = rpid[i] >= 0 ? wait(rpid[i]) : -1;
        int bad_pair = wpid[i] < 0 || rpid[i] < 0 || w != kib || r != kib;

        /* A pair that never got both of its children is a resource failure
         * (exec could not find a window), not a pipe failure - say which,
         * because the numbers look the same either way. */
        if (wpid[i] < 0 || rpid[i] < 0)
            printf("  p%d: exec failed (w=%d r=%d)\n", i, wpid[i], rpid[i]);
        else
            printf("  p%d: sent %d KiB, verified %d KiB%s\n", i, w, r,
                   bad_pair ? "  <- FAIL" : "");
        if (bad_pair)
            bad = 1;
    }
    puts(bad ? "piperace: FAIL\n" : "piperace: ok\n");
    return bad;
}
