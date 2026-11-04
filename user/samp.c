/* samp - what does a 1 ms sampling loop actually deliver?
 *
 * Sleeps a millisecond at a time and records how much the clock advanced,
 * so the answer comes from the hardware counter rather than from what the
 * tick rate is supposed to be.  A tick-quantised sleeper cannot do better
 * than "one tick late": the histogram says whether that is 1 ms, 2 ms or
 * worse on this board.
 */
#include "lib.h"

#define N 1000

int main(void)
{
    int prev = clock_ms();
    int one = 0, two = 0, three = 0, more = 0, worst = 0;

    for (int i = 0; i < N; i++) {
        sleep(1);
        int now = clock_ms();
        int d = now - prev;
        prev = now;
        if (d <= 1)
            one++;
        else if (d == 2)
            two++;
        else if (d == 3)
            three++;
        else
            more++;
        if (d > worst)
            worst = d;
    }

    printf("samp: <=1ms %d  2ms %d  3ms %d  >3ms %d  worst %d ms\n",
           one, two, three, more, worst);
    return 0;
}
