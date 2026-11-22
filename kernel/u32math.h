#ifndef TINYOS_U32MATH_H
#define TINYOS_U32MATH_H

#include "types.h"

/* Unchecked round-up.  Callers must know a == 0 cannot happen; the exec path
 * uses align_up_ok() instead, since every number there is untrusted. */
static inline uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Checked forms used on the exec path: every number below comes from an
 * untrusted envelope, and an unchecked wrap here turns an undersized window
 * into an out-of-bounds decompressor write. */
static inline int add_u32_ok(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a > 0xFFFFFFFFu - b)
        return -1;
    *out = a + b;
    return 0;
}

static inline int align_up_ok(uint32_t v, uint32_t a, uint32_t *out)
{
    uint32_t rounded;

    if (a == 0 || (a & (a - 1u)) != 0)
        return -1;
    if (v > 0xFFFFFFFFu - (a - 1u))
        return -1;
    rounded = (v + a - 1u) & ~(a - 1u);
    if (rounded < v)
        return -1;
    *out = rounded;
    return 0;
}

#endif /* TINYOS_U32MATH_H */
