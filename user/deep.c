#include "lib.h"

/* Recursion deeper than the default stack, on purpose, without asking for a
 * bigger one.
 *
 *   deep [levels] [ms] [heap KiB]
 *
 * `stk` proves a program can *declare* the stack it needs; this one proves
 * the other half.  Nothing is declared here, so the task starts with the
 * board default (8 KiB on the C3) and runs off the bottom of it.  Instead of
 * killing it, the kernel doubles the stack into the heap's unused tail and
 * retries the instruction that faulted - `ps` then shows 12.x/16 for this
 * task, and the line printed at the fault says what the heap gave up.
 *
 * Growth needs somewhere to grow into: a window is sized from the image, so a
 * program with nothing in it gets 16 KiB and no slack at all.  The journal
 * below is what makes this one hold 32 KiB - the same way a real program with
 * real data would - and it doubles as a check that the deeper stack stayed
 * where it belongs.  If the doubling had run into the heap, the journal would
 * be the first casualty.
 *
 * Depth is the first argument.  One growth step is what an undeclared stack
 * gets (plat_window_grow_reserve), so on the C3 - 4 KiB default - `deep 3`
 * (6 KiB) is the run that grows, 4 -> 8 KiB, and `deep 6` (12 KiB) shows the
 * other side: a second step has nowhere to come from and the task is killed.
 * A program that knows it goes deeper should say so with TOS_STACK (`stk`
 * does) instead of relying on steps.
 * A third argument takes the slack first - `deep 6 0 8` mallocs 8 KiB, which
 * is what the stack would have grown into, and the overflow then kills the
 * task as it always did.  That is the other side of sharing one region: the
 * heap gets there first, and the stack has to say so up front (TOS_STACK) if
 * it cannot afford to lose.
 */

#define LEVELS       6
#define FRAME_BYTES  2048

/* One byte per level, a KiB apart, so a stack that reached this block would
 * land between two slots instead of on one and be visible as damage. */
static char journal[LEVELS * 1024];

static int recurse(int level, int ms)
{
    volatile char frame[FRAME_BYTES];

    /* Same escape hatch as stk.c: without it the optimiser may turn 2 KiB of
       frame into two scalars, and then there is nothing here to overflow. */
    __asm__ __volatile__("" : : "r"(frame) : "memory");

    frame[0] = (char)level;
    frame[FRAME_BYTES - 1] = (char)level;
    journal[(level - 1) * 1024] = (char)level;

    if (level > 1)
        recurse(level - 1, ms);
    if (level == 1 && ms > 0)
        sleep(ms);          /* parked in the deepest frame: ps reads it live */

    /* Touch the frame again on the way out, so the high-water mark is the
       deepest frame rather than whatever the last level happened to write. */
    return frame[0] ^ frame[FRAME_BYTES - 1];
}

int main(int argc, char **argv)
{
    int levels = (argc > 1) ? atoi(argv[1]) : LEVELS;
    int ms = (argc > 2) ? atoi(argv[2]) : 3000;
    int kib = (argc > 3) ? atoi(argv[3]) : 0;
    int bad = 0;

    /* Take the heap's slack before recursing, so the stack has nowhere to
       grow: malloc() moves the break, and that is the number the growth
       check compares against. */
    for (int i = 0; i < kib; i++) {
        if (!malloc(1024)) {
            printf("deep: malloc stopped after %d KiB\n", i);
            break;
        }
    }

    if (levels < 1)
        levels = LEVELS;
    if (levels > LEVELS)
        levels = LEVELS;        /* one journal slot per level */
    if (ms < 0)
        ms = 0;

    recurse(levels, ms);

    for (int i = 0; i < levels; i++) {
        if (journal[i * 1024] != (char)(i + 1))
            bad++;
    }

    printf("deep: %d levels, journal %s\n", levels, bad ? "damaged" : "intact");
    return bad ? 1 : 0;
}
