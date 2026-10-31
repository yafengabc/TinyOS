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

    puts("writing one byte past the heap end\n");
    /* Ours only once sbrk() says so; until then it belongs to nobody. */
    p[0] = 1;
}

static void stack(void)
{
    volatile char probe;
    uint32_t here = (uint32_t)&probe;   /* somewhere near the stack top */

    puts("writing below the bottom of the stack\n");
    /* 32 KiB of stack, then the hole the kernel keeps under it, so this
       lands in memory nobody owns.  (Asking for a 64 KiB local array would
       be simpler but the optimiser throws the array away.) */
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
