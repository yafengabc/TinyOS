#include "lib.h"
#include "fix16.h"
#if defined(TOS_SOFTFLOAT)
#include "softfloat32.h"
#endif

/* Side-by-side demo of the two numeric runtimes.
 *
 * Q16.16 is always linked and is the default.  The float column exists only
 * when the build enables [features] softfloat = true, which adds the 32-bit
 * soft-float runtime to the image.  The table keeps both columns so the
 * comparison is still visible when it is on. */

struct pair {
    float f;
    fix16_t q;
    const char *name;
};

static const struct pair cases[] = {
    { 1.5f,          98304,    "1.5" },
    { -2.25f,       -147456,   "-2.25" },
    { 3.14159265f,   205887,   "pi" },
    { 0.1f,          6554,     "0.1" },
    { -0.0009765625f, -64,     "-2^-10" },
    { 123.456f,      8090780,  "123.456" },
    { 1000.5f,       65568768, "1000.5" },
    { 1.0f / 3.0f,   21845,    "1/3" },
};

#define N 50000

int main(void)
{
    volatile fix16_t qa = FIX16_ONE;
#if defined(TOS_SOFTFLOAT)
    volatile float fa = 1.0f;
#endif
    int i, t0, t1;

    puts("float backend: ");
#if defined(TOS_SOFTFLOAT_MIN)
    puts("IEEE binary32 (min)\n");
#elif defined(TOS_SOFTFLOAT)
    puts("IEEE binary32\n");
#else
    puts("fix16 only\n");
#endif

    puts("value\tfix16");
#if defined(TOS_SOFTFLOAT)
    puts("\tfloat");
#endif
    puts("\n");
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        puts(cases[i].name);
        puts("\t");
        printfix16(cases[i].q);
#if defined(TOS_SOFTFLOAT)
        puts("\t");
        printfloat(cases[i].f);
#endif
        puts("\n");
    }

    puts("\n0.1 added ten times\n");
    {
        fix16_t q = 0;

        for (i = 0; i < 10; i++)
            q = fix16_add(q, 6554);
        puts("fix16 ");
        printfix16(q);
#if defined(TOS_SOFTFLOAT)
        {
            float f = 0.0f;

            for (i = 0; i < 10; i++)
                f = f + 0.1f;
            puts("   float ");
            printfloat(f);
        }
#endif
        puts("\n");
    }

    puts("\n1/3 times three\n");
    {
        fix16_t q = fix16_div(FIX16_ONE, 3 * FIX16_ONE);

        q = fix16_mul(q, 3 * FIX16_ONE);
        puts("fix16 ");
        printfix16(q);
#if defined(TOS_SOFTFLOAT)
        {
            float f = __divsf3(1.0f, 3.0f);

            f = f * 3.0f;
            puts("   float ");
            printfloat(f);
        }
#endif
        puts("\n");
    }

    puts("\nbench ");
    putint(N);
    puts(" x (v*1.0001 - 0.0001)\n");

    t0 = uptime();
    for (i = 0; i < N; i++)
        qa = fix16_sub(fix16_mul(qa, 65543), 7);
    t1 = uptime();
    puts("fix16 ");
    putint(t1 - t0);
    puts(" ms  result ");
    printfix16(qa);
    puts("\n");

#if defined(TOS_SOFTFLOAT)
    t0 = uptime();
    for (i = 0; i < N; i++)
        fa = fa * 1.0001f - 0.0001f;
    t1 = uptime();
    puts("float ");
    putint(t1 - t0);
    puts(" ms  result ");
    printfloat(fa);
    puts("\n");
#endif
    return 0;
}
