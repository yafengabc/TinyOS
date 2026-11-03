#include "lib.h"

/* Prove that a declared heap is a floor the kernel honoured, not a wish.
 *
 *     TOS_HEAP(n) puts a note in the image; exec() adds n to the window
 *     cost before it picks one, so this program - a few hundred bytes of
 *     image - gets a 32 KiB window on the C3 instead of the 16 KiB a bare
 *     image of this size would take, and 12 KiB of those are guaranteed to
 *     sit between the break and the stack.
 *
 * The test is the part that cannot pass by accident: take the whole
 * declared heap with malloc(), write a pattern over every page of it, and
 * read the pattern back.  The write matters - malloc only moves the break,
 * so a break that moved into the guard hole, or into the stack, would
 * still return success until the first store, and the first store into a
 * hole nobody owns faults.  Reading back catches the subtler failure of a
 * region that aliases something else.
 *
 * The size is chosen to straddle the rounding: 12 KiB of heap + 8 KiB of
 * stack + 4 KiB of guard + one page of image is 25 KiB, which rounds to a
 * 32 KiB window - so the request is what pushed the window up a step, and
 * `ps` shows it in the wink column. */

TOS_HEAP(12 * 1024);

#define WANT (12 * 1024)

int main(int argc, char **argv)
{
    int ms = (argc > 1) ? atoi(argv[1]) : 0;
    unsigned char *p = malloc(WANT);

    if (!p) {
        printf("wheap: malloc(%d) failed\n", WANT);
        return 1;
    }
    for (int i = 0; i < WANT; i++)
        p[i] = (unsigned char)(i * 7u + (i >> 9));
    for (int i = 0; i < WANT; i++) {
        if (p[i] != (unsigned char)(i * 7u + (i >> 9))) {
            printf("wheap: byte %d read back wrong\n", i);
            return 1;
        }
    }
    printf("wheap: %d KiB declared, %d KiB taken, pattern intact\n",
           WANT / 1024, WANT / 1024);
    /* Park on request so `ps` can read the window this program holds: the
       note is what sized it, and the wink column is the receipt. */
    if (ms > 0)
        sleep(ms);
    return 0;
}
