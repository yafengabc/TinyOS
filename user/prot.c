#include "lib.h"

/* Break the task's memory protection on purpose, one rule at a time.
 *
 * On a board with PMP each case dies with a trap naming the fault, and the
 * shell carries on: a broken task is not a broken machine.  On the x86 port
 * there is no MMU, so nothing happens and the case reports that it survived
 * - which is the honest answer for a machine with no way to say no.
 *
 * usage: prot rodata | heap | stack | null
 */

static void rodata(void)
{
    puts("writing through a pointer to .rodata\n");
    /* Below the writable split: readable, executable, not writable. */
    *(volatile char *)"read-only" = 'R';
}

static void heap(void)
{
    volatile char *p = sbrk(0);

    puts("writing past the end of the heap\n");
    /* Ours only once sbrk() says so; until then it belongs to nobody.
       64 bytes clear of the break so the answer does not depend on how
       coarse the machine's regions are: MPU regions are 32-byte granular
       and the heap is described up to the break rounded up, PMP entries are
       4-byte granular. */
    p[64] = 1;
}

static void stack(void)
{
    volatile char probe;
    uint32_t here = (uint32_t)&probe;   /* somewhere near the stack top */

    puts("writing below the bottom of the stack\n");
    /* 40 KiB below the top clears the largest stack any board hands out
       (32 KiB, plus the guard under it), so this is memory the task does not
       own: the hole under the stack where the window has room for it,
       another task's window where it does not.  Either way it is outside
       every PMP entry this task has, which is what the test needs.
       (Asking for a 64 KiB local array would be simpler but the optimiser
       throws the array away.) */
    *(volatile char *)(here - 40960u) = 1;
}

static void null(void)
{
    puts("reading address zero\n");
    (void)*(volatile int *)0;
}

int main(int argc, char *argv[])
{
    if (argc > 1) {
        switch (argv[1][0]) {
        case 'r': rodata(); break;
        case 'h': heap();   break;
        case 's': stack();  break;
        case 'n': null();   break;
        default:  argc = 1;
        }
        if (argc > 1) {
            puts("survived: this machine cannot stop us\n");
            return 0;
        }
    }
    puts("prot rodata|heap|stack|null - break one rule\n");
    return 0;
}
