/* rawsink - how much of a burst survives while the reader is away?
 *
 * The raw ring only fills while the reader is *not* reading, so the only
 * question worth asking is "how much land while I am gone".  This program
 * answers it directly rather than by arithmetic: the host sends a burst of
 * a known size, this program sleeps through it, and then drains whatever
 * survived.  The total it reports *is* the buffer, hardware FIFO included.
 *
 *     rawsink <stall_ms> <expect_bytes>
 *
 * The burst is a counting pattern of printable bytes, so a gap is visible as
 * a skipped value as well as a short total - losing exactly 95n bytes would
 * look continuous, and the total catches that case.
 *
 * '\n' ends the drain: the pattern never contains it, so the host can wake a
 * blocked reader without the stop byte being mistaken for data.
 */
#include "lib.h"

#define SEQ_LO ' '
#define SEQ_HI '~'
#define SEQ_N  (SEQ_HI - SEQ_LO + 1)

int main(int argc, char **argv)
{
    int stall = argc > 1 ? atoi(argv[1]) : 0;
    int want  = argc > 2 ? atoi(argv[2]) : 4096;
    char buf[64];
    int expect = -1;
    int total = 0, reads = 0, gaps = 0, lost = 0, worst = 0;

    printf("READY %d %d\n", stall, want);
    ttyraw(1);

    /* Go away before reading anything: the ring starts empty (the console
     * reader is only just attached), so everything that arrives from here on
     * has to fit in ring + FIFO. */
    if (stall > 0)
        sleep(stall);

    int t0 = clock_ms();
    while (total < want && clock_ms() - t0 < 4000) {
        int n = read(0, buf, sizeof buf);
        if (n <= 0)
            break;
        reads++;
        for (int i = 0; i < n; i++) {
            int c = (unsigned char)buf[i];
            if (c == '\n')
                goto done;
            if (expect >= 0) {
                int next = expect >= SEQ_HI ? SEQ_LO : expect + 1;
                if (c != next) {
                    int miss = (c - next + SEQ_N) % SEQ_N;
                    gaps++;
                    lost += miss;
                    if (miss > worst)
                        worst = miss;
                }
            }
            expect = c;
        }
        total += n;
    }
done:
    ttyraw(0);
    printf("rawsink: stall %d ms  want %d  reads %d  got %d  gaps %d  lost %d  worst %d\n",
           stall, want, reads, total, gaps, lost, worst);
    return 0;
}
