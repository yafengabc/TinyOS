#include "lib.h"

/* Time since boot, from the real clock rather than the scheduler tick: the
 * millisecond field is meaningful because plat_now_ns() reads a free-running
 * hardware counter. */
int main(void)
{
    struct clockinfo c;
    uint32_t ms, frac;

    clockinfo(&c);
    ms = c.ms;
    frac = ms % 1000;

    puts("uptime: ");
    putint((int)(ms / 1000));
    putchar('.');
    if (frac < 100)
        putchar('0');
    if (frac < 10)
        putchar('0');
    putint((int)frac);
    puts(" s (");
    putint((int)ms);
    puts(" ms)  tick=");
    putint((int)c.tick_hz);
    puts(" Hz  cpus=");
    putint((int)c.cpus);
    puts("  cpu=");
    putint(getcpu());
    puts("  pid=");
    putint(getpid());
    puts("\n");
    return 0;
}
