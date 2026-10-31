#include "types.h"

/* Integer helpers normally provided by compiler-rt.
 *
 * RV32IM does have MUL/MULH, but a generic rv32 target may still emit calls
 * for 64-bit operations, and the hard-float compiler_rt from zig cannot be
 * linked (ABI mismatch).  Implementing them here keeps the kernel free of
 * any external runtime. */

#if !defined(__i386__) && !defined(__x86_64__)

uint32_t __mulsi3(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    while (b) {
        if (b & 1)
            r += a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

uint32_t __udivsi3(uint32_t n, uint32_t d)
{
    if (d == 0)
        return 0;
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (1u << i);
        }
    }
    return q;
}

uint32_t __umodsi3(uint32_t n, uint32_t d)
{
    if (d == 0)
        return 0;
    uint32_t r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d)
            r -= d;
    }
    return r;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    if (d == 0)
        return 0;
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= ((uint64_t)1 << i);
        }
    }
    return q;
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    if (d == 0)
        return 0;
    uint64_t r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d)
            r -= d;
    }
    return r;
}

uint64_t __muldi3(uint64_t a, uint64_t b)
{
    uint64_t r = 0;
    while (b) {
        if (b & 1)
            r += a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

#endif
