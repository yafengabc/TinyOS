#include "lib.h"

/* Wait, without using the CPU.
 *
 * Milliseconds, not seconds: the kernel ABI, the tick that wakes a sleeper,
 * samp's sampling interval and deep's second argument are all milliseconds,
 * so a `sleep` in seconds would be the one odd unit on the console.
 *
 * The console's `sleep` is this program, not a shell builtin, so it
 * composes like everything else the shell starts: `sleep 500 &` is a
 * background task the scheduler can be watched against, `nice 0 sleep 500 &`
 * is a high-priority one, and ^C has something to interrupt (the shell is
 * waiting on a real child, and init itself is deliberately not a signal
 * target).  Inside a script `sleep N` is handled by sh with a bare
 * SYS_SLEEP instead, because a sampling loop cannot pay for an exec per
 * sample; `run sleep N` and `sleep N &` still land here.
 *
 * The upper bound is checked here rather than left to the kernel, which
 * clamps at an hour: a silent clamp turns `sleep 5400000` into a lie about
 * how long the guest was idle, and this is a tree that measures things. */

#define SLEEP_MAX_MS 3600000        /* the kernel's clamp, stated instead */

static int parse_ms(const char *s, int *out)
{
    long v = 0;

    if (*s == '\0')
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
        if (v > SLEEP_MAX_MS)
            break;                  /* keep scanning: "3600000x" is invalid */
    }
    if (*s != '\0' || v > SLEEP_MAX_MS)
        return -2;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv)
{
    int ms;
    int rc;

    if (argc < 2) {
        puts("usage: sleep <ms>   (max 3600000)\n");
        return 1;
    }
    rc = parse_ms(argv[1], &ms);
    if (rc == -1) {
        puts("sleep: not a number of milliseconds: ");
        puts(argv[1]);
        puts("\n");
        return 1;
    }
    if (rc == -2) {
        puts("sleep: longer than 3600000 ms: ");
        puts(argv[1]);
        puts("\n");
        return 1;
    }
    sleep(ms);
    return 0;
}
