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

/* 64-bit shifts.  RV32 has no shift wider than XLEN, so clang turns every
 * 64-bit shift into a call - including the ones inside the routines above,
 * which is why these have to work on the two 32-bit halves directly. */
typedef union {
    uint64_t u;
    uint32_t w[2];              /* w[0] = low half (little endian) */
    int32_t  i[2];
} dword_t;

uint64_t __lshrdi3(uint64_t v, int n)
{
    dword_t d;
    d.u = v;
    if (n <= 0)
        return v;
    if (n >= 64)
        return 0;
    if (n >= 32) {
        d.w[0] = d.w[1] >> (n - 32);
        d.w[1] = 0;
    } else {
        d.w[0] = (d.w[0] >> n) | (d.w[1] << (32 - n));
        d.w[1] >>= n;
    }
    return d.u;
}

uint64_t __ashldi3(uint64_t v, int n)
{
    dword_t d;
    d.u = v;
    if (n <= 0)
        return v;
    if (n >= 64)
        return 0;
    if (n >= 32) {
        d.w[1] = d.w[0] << (n - 32);
        d.w[0] = 0;
    } else {
        d.w[1] = (d.w[1] << n) | (d.w[0] >> (32 - n));
        d.w[0] <<= n;
    }
    return d.u;
}

uint64_t __ashrdi3(uint64_t v, int n)
{
    dword_t d;
    d.u = v;
    if (n <= 0)
        return v;
    if (n >= 64)
        return (uint64_t)(d.i[1] < 0 ? -1 : 0);
    if (n >= 32) {
        d.i[0] = d.i[1] >> (n - 32);
        d.i[1] = d.i[1] < 0 ? -1 : 0;
    } else {
        d.i[0] = (int32_t)((d.w[0] >> n) | (d.w[1] << (32 - n)));
        d.i[1] >>= n;
    }
    return d.u;
}

#endif
