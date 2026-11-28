#include "lib.h"

/* Recursion past the stack, on purpose, to prove the kill path.
 *
 *   deep [levels] [ms]
 *
 * `stk` is the other half of the stack story: it declares TOS_STACK(16 * 1024)
 * and stays inside it.  This one declares nothing, so the task gets the board
 * default (4 KiB on the 4 KiB-grain boards) and runs off the bottom.  Since
 * 2026-11-28 there is no implicit growth: the guard fault (or the software
 * guard's sp check, on the ports that have one) reaches process_stack_grow(),
 * which finds no reserve, prints "declare TOS_STACK() for the depth you need"
 * and the task is killed.
 *
 * The run therefore ends with the kernel's kill message, not with a print from
 * this program: a killed task cannot report its journal.  `deep 6` (12 KiB)
 * exercises the same path two levels further down.
 */

#define LEVELS       6
#define FRAME_BYTES  2048

static int recurse(int level, int ms)
{
    volatile char frame[FRAME_BYTES];

    /* Same escape hatch as stk.c: without it the optimiser may turn 2 KiB of
       frame into two scalars, and then there is nothing here to overflow. */
    __asm__ __volatile__("" : : "r"(frame) : "memory");

    frame[0] = (char)level;
    frame[FRAME_BYTES - 1] = (char)level;

    if (level > 1)
        recurse(level - 1, ms);
    if (level == 1)
        sleep(ms);          /* parked in the deepest frame, so a software guard
                             * sees sp outside the stack at a syscall instead of
                             * the write having gone unnoticed */

    /* Touch the frame on the way out, so the high-water mark is the deepest
       frame rather than whatever the last level happened to write. */
    return frame[0] ^ frame[FRAME_BYTES - 1];
}

int main(int argc, char **argv)
{
    int levels = (argc > 1) ? atoi(argv[1]) : LEVELS;
    int ms = (argc > 2) ? atoi(argv[2]) : 3000;

    if (levels < 1)
        levels = LEVELS;
    if (levels > LEVELS)
        levels = LEVELS;
    if (ms < 0)
        ms = 0;

    recurse(levels, ms);
    return 0;
}
