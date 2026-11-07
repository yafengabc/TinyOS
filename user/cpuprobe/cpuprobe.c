/* cpuprobe - count what getcpu() actually returns.
 *
 * The SMP worker in user/smp.c reported "cpu 0 then 25" on the RP2350,
 * where 25 was a pid.  Either the syscall returns a stale value under
 * load, or the frame the kernel writes r0 into is not the one the task
 * resumes from.  This program does nothing but ask, so the answer cannot
 * be confused with printf, exec or the scheduler.
 *
 *   cpuprobe           ask N times, print the histogram
 *   cpuprobe <n>       same, with a different N
 *
 * Run it twice at once (cpuprobe & cpuprobe &) to get the two cores
 * fighting over the run queue: a uniprocessor build always answers 0.
 */
#include "lib.h"

TOS_STACK(4 * 1024);

#define ASKS 4000

int main(int argc, char *argv[])
{
    int n = ASKS;
    int c0 = 0, c1 = 0, bad = 0;
    int first_bad = -1;
    int bad_pid = 0;

    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v > 0)
            n = v;
    }

    int me = getpid();

    for (int i = 0; i < n; i++) {
        int c = getcpu();

        if (c == 0) {
            c0++;
        } else if (c == 1) {
            c1++;
        } else {
            bad++;
            if (first_bad < 0)
                first_bad = c;
            /* The interesting collision: the value is a pid, not a cpu. */
            if (c == me)
                bad_pid++;
        }
    }

    puts("cpuprobe: pid ");
    putint(me);
    puts(" n=");
    putint(n);
    puts(" cpu0=");
    putint(c0);
    puts(" cpu1=");
    putint(c1);
    puts(" bad=");
    putint(bad);
    if (bad) {
        puts(" first=");
        putint(first_bad);
        puts(" of_which_self=");
        putint(bad_pid);
    }
    putchar('\n');

    return bad ? 1 : 0;
}
