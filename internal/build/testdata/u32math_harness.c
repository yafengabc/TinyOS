/* Host harness for kernel/u32math.h.
 *
 * Every one of these numbers comes from an untrusted envelope on the exec
 * path; the cases are the wrap points, not the arithmetic.
 */
#include <stdio.h>

#include "u32math.h"

static int failures;

static void want_u32(const char *what, uint32_t got, uint32_t expect)
{
    if (got != expect) {
        printf("FAIL %s: got %#x want %#x\n", what, got, expect);
        failures++;
    }
}

static void want_int(const char *what, int got, int expect)
{
    if (got != expect) {
        printf("FAIL %s: got %d want %d\n", what, got, expect);
        failures++;
    }
}

int main(void)
{
    want_u32("align_up already aligned", align_up(0x1000, 0x1000), 0x1000);
    want_u32("align_up rounds up",       align_up(0x1001, 0x1000), 0x2000);
    want_u32("align_up zero",            align_up(0, 0x1000), 0);
    want_u32("align_up 64k",             align_up(1, 0x10000), 0x10000);

    uint32_t out = 0;
    want_int("add at the limit",   add_u32_ok(0xfffffffeu, 1, &out), 0);
    want_u32("add at the limit out", out, 0xffffffffu);
    out = 0xdead;
    want_int("add one too many",   add_u32_ok(0xffffffffu, 1, &out), -1);
    want_u32("out untouched on fail", out, 0xdead);
    want_int("add zero",           add_u32_ok(0, 0, &out), 0);
    want_u32("add zero out",       out, 0);

    want_int("align_up_ok a = 0",        align_up_ok(5, 0, &out), -1);
    want_int("align_up_ok a = 3",        align_up_ok(5, 3, &out), -1);
    want_int("align_up_ok high page",    align_up_ok(0xfffff000u, 0x1000, &out), 0);
    want_u32("align_up_ok high out",     out, 0xfffff000u);
    want_int("align_up_ok would wrap",   align_up_ok(0xfffff001u, 0x1000, &out), -1);
    want_int("align_up_ok rounds",       align_up_ok(0x1001, 0x1000, &out), 0);
    want_u32("align_up_ok rounds out",   out, 0x2000);

    if (failures == 0) {
        printf("u32math ok\n");
    }
    return failures ? 1 : 0;
}
