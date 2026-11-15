#include "lib.h"

/* Break the task's memory protection on purpose, one rule at a time.
 *
 * On a board with PMP each case dies with a trap naming the fault, and the
 * shell carries on: a broken task is not a broken machine.  On the x86 port
 * there is no MMU, so nothing happens and the case reports that it survived
 * - which is the honest answer for a machine with no way to say no.
 *
 * usage: prot rodata | heap | stack | null
 *        prot dev <addr>          write one word to <addr> from U-mode
 *
 * `dev` exists for one question the others cannot answer: on the RP2350 the
 * bootrom leaves PMP entries hardwired RWX, and if they cover peripherals then
 * user mode can write device registers.  Pass the address, so the test needs
 * no board knowledge:
 *
 *   prot dev 0x40070000      RP2350/RP2040 UART0 data register
 *   prot dev 0x60000000      ESP32-C3 UART0 FIFO
 *   prot dev 0x50200000      RP2350 PIO0 (should never be reachable)
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

/* Parse 0x-hex or decimal; returns 0 on garbage. */
static int parse_addr(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int base = 10;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s)
        return 0;
    for (; *s; s++) {
        int d;

        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else
            return 0;
        v = v * (uint32_t)base + (uint32_t)d;
    }
    *out = v;
    return 1;
}

/* One MMIO word from user mode.  A board that protects its peripherals faults
 * here; one that does not survives, and that is the answer we came for. */
static void dev(uint32_t addr)
{
    puts("writing one word to a device register\n");
    *(volatile uint32_t *)addr = 1u;
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
        case 'd': {
            uint32_t a;

            if (argc > 2 && parse_addr(argv[2], &a))
                dev(a);
            else
                argc = 1;           /* usage */
            break;
        }
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
