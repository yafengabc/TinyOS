#include "lib.h"
#include "fix16.h"
#include "softfloat32.h"

/* Side-by-side demo of the two numeric runtimes.
 *
 * Both are always linked.  `float` values are IEEE binary32 (or the
 * saturating softfloat-min variant, if the build selected it); the fix16
 * column is the explicit Q16.16 API.  Neither path converts at run time. */

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
    volatile float fa = 1.0f;
    int i, t0, t1;

    puts("softfloat: ");
#if defined(TOS_SOFTFLOAT_MIN)
    puts("min (no NaN/Inf, flush to zero)\n");
#else
    puts("IEEE binary32\n");
#endif

    puts("value\tfix16\tfloat\n");
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        puts(cases[i].name);
        puts("\t");
        printfix16(cases[i].q);
        puts("\t");
        printfloat(cases[i].f);
        puts("\n");
    }

    puts("\n0.1 added ten times\n");
    {
        fix16_t q = 0;
        float f = 0.0f;

        for (i = 0; i < 10; i++) {
            q = fix16_add(q, 6554);
            f = f + 0.1f;
        }
        puts("fix16 ");
        printfix16(q);
        puts("   float ");
        printfloat(f);
        puts("\n");
    }

    puts("\n1/3 times three\n");
    {
        fix16_t q = fix16_div(FIX16_ONE, 3 * FIX16_ONE);
        float f = 1.0f / 3.0f;

        q = fix16_mul(q, 3 * FIX16_ONE);
        f = f * 3.0f;
        puts("fix16 ");
        printfix16(q);
        puts("   float ");
        printfloat(f);
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

    t0 = uptime();
    for (i = 0; i < N; i++)
        fa = fa * 1.0001f - 0.0001f;
    t1 = uptime();
    puts("float ");
    putint(t1 - t0);
    puts(" ms  result ");
    printfloat(fa);
    puts("\n");
    return 0;
}
