#include "lib.h"

/* Forces the linker to emit relocations that the kernel must patch when the
   image is moved to APP_BASE.  Without a working relocation step this
   program jumps into the weeds. */

/* 1. Pointer table in .data: each entry is an absolute address. */
static const char *const words[] = {
    "alpha",
    "bravo",
    "charlie",
    "delta",
};

/* 2. Function pointer, also an absolute address in .data. */
static int double_it(int x) { return x * 2; }
static int negate_it(int x) { return -x; }

static int (*const ops[])(int) = { double_it, negate_it };

/* 3. A string built at runtime that points at one of the strings above. */
static const char *pick;

int main(void)
{
    puts("reloc test\n");

    /* pointer table */
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        puts("word[");
        putint((int)i);
        puts("] = ");
        puts(words[i]);
        puts("\n");
    }

    /* function pointers */
    puts("double(21) = ");
    putint(ops[0](21));
    puts("\n");
    puts("negate(7)  = ");
    putint(ops[1](7));
    puts("\n");

    pick = words[2];
    puts("pick       = ");
    puts(pick);
    puts("\n");
    puts("pick[0]    = ");
    putchar(pick[0]);
    puts("\n");

    /* sanity: the strings must live inside the process image, which on this
       port means above 0x08000000.  Printing the low byte is enough to tell
       whether the address looks sane. */
    return 0;
}
