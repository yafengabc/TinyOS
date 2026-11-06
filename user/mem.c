#include "lib.h"

/* A heap the size of the window's spare room, written to and read back: the
 * point is that sbrk() moves the break and the protection moves with it, so
 * the pages the malloc returned are writable and nobody else's.
 *
 * WANT is also the declared floor, which is what makes the window big enough
 * to hold it - see TOS_HEAP in lib.h.  Without the note a window is image +
 * stack + guard and the heap is zero.  It used to ask for 200000 bytes, which
 * no window on this chip can hold next to a running shell; 64 KiB is enough
 * to walk 16 pages and still fits everywhere. */
#define WANT (64 * 1024)

TOS_HEAP(WANT);

int main(int argc, char *argv[])
{
    int want = WANT;

    if (argc > 1) {
        int v = atoi(argv[1]) * 1024;
        if (v > 0 && v <= WANT)
            want = v;
    }

    puts("heap demo, pid=");
    putint(getpid());
    puts(", asking ");
    putint(want);
    puts(" bytes\n");

    char *p = malloc(want);
    if (!p) {
        puts("malloc failed\n");
        return 1;
    }

    for (int i = 0; i < want; i += 4096)
        p[i] = (char)(i / 4096);

    int sum = 0;
    for (int i = 0; i < want; i += 4096)
        sum += p[i];

    puts("touched ");
    putint(want / 4096);
    puts(" pages, checksum=");
    putint(sum);
    puts("\n");
    free(p);
    puts("done\n");
    return 0;
}
