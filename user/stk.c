#include "lib.h"

/* Burn a known amount of stack, then park so `ps` can read it from outside.
 *
 *   stk [depth] [ms]
 *
 * Every level holds 2 KiB of local storage, so `stk 4 5000 &` followed by `ps`
 * must show roughly 8 KiB in the stk column.  That is the point of this
 * program: the high-water mark in ps comes from exec() filling the stack with
 * 0xCC and scanning up from the bottom for the deepest byte that changed, and
 * a number that merely looks plausible is not evidence that the scan measures
 * the right thing.  A depth whose frames are allotted by the compiler but
 * never touched would read as unused; writing the first and last byte of each
 * buffer keeps that from being true by accident.
 *
 * The buffers are volatile so no optimisation level can decide they are dead
 * and drop two thirds of the frames.  The tightest budget this can produce is
 * depth * 2 KiB, which is why the tests use a depth well under the stack:
 * overflow here would be measuring the guard page, not the stack.
 *
 * This is also the program that proves a task can ask for its own stack: the
 * board default is 8 KiB and six frames do not fit in that, so the note below
 * asks for 16 and exec() sizes this task's window accordingly.  Without it
 * this task would depend on the *other* mechanism (process_stack_grow): it
 * would fault at 8 KiB and be handed 16 only if its window still had heap
 * slack.  user/deep.c is that path, and it is why this one keeps the note. */

TOS_STACK(16 * 1024);

static int burn(int depth, int ms)
{
    volatile char buf[2048];

    /* Without this the compiler is free to notice that only two bytes of the
       array are ever read and replace 2 KiB of frame with two scalars - which
       it did, and ps then reported 0.3 KiB for a 8 KiB recursion.  Handing
       the address to an empty asm statement makes the object escape, so it
       has to exist in the frame it was declared in. */
    __asm__ __volatile__("" : : "r"(buf) : "memory");

    buf[0] = (char)depth;
    buf[sizeof(buf) - 1] = (char)depth;
    if (depth > 1)
        return burn(depth - 1, ms) + buf[0];
    if (ms > 0)
        sleep(ms);
    return buf[sizeof(buf) - 1];
}

int main(int argc, char **argv)
{
    int depth = (argc > 1) ? atoi(argv[1]) : 4;
    int ms = (argc > 2) ? atoi(argv[2]) : 3000;

    if (depth < 1)
        depth = 1;
    /* 6 frames of 2 KiB is 12 KiB: below the 16 KiB this program asks for,
       and deep enough that a measurement which misses whole frames is
       obvious. */
    if (depth > 6)
        depth = 6;
    if (ms < 0)
        ms = 0;
    return burn(depth, ms) & 0x7f;
}
