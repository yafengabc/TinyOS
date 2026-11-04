/* samp - what does a 1 ms sampling loop actually deliver?
 *
 * Sleeps to an absolute deadline a millisecond at a time and records how far
 * apart the samples landed, so the answer comes from the hardware counter
 * rather than from what the tick rate is supposed to be.  A tick-quantised
 * sleeper cannot do better than "one tick late": the histogram says whether
 * that is 1 ms, 2 ms or worse on this board.
 *
 * The deadline is why this measures the scheduler and not arithmetic.  With
 * `sleep(1)` at the end of the body, the wake-up is one tick after whatever
 * the clock said *when the call was reached* - and a tick landing between the
 * read and the call quietly costs a whole millisecond, about 1% of samples.
 * Accumulating the deadline (`next += 1`) never drifts: a late sample is one
 * late sample, not a phase shift.
 */
#include "lib.h"

#define N 1000

int main(void)
{
    int prev = clock_ms();
    int next = prev;
    int one = 0, two = 0, three = 0, more = 0, worst = 0;

    for (int i = 0; i < N; i++) {
        int wait;

        next += 1;
        while ((wait = next - clock_ms()) > 0)
            sleep(wait);

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
