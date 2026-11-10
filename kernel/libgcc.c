#include "types.h"

/* Integer helpers normally provided by compiler-rt.
 *
 * RV32IM does have MUL/MULH, but a generic rv32 target may still emit calls
 * for 64-bit operations, and the hard-float compiler_rt from zig cannot be
 * linked (ABI mismatch).  Implementing them here keeps the kernel free of
 * any external runtime. */

/* Every port gets them: -O2 folds the kernel's 64-bit divisions into
 * multiply/shift sequences, but -Oz emits calls to these even on x86, and
 * --gc-sections drops whatever a build does not use. */

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

#if defined(__arm__)

/* ARM EABI helpers clang emits for Cortex-M.  The M3 has no hardware
 * divide, so every 32-bit / and % becomes a call, and struct copies use
 * the alignment-specific memcpy entry points.  libc lives in
 * kernel/string.c, so these are thin aliases plus one division routine;
 * without them lld reports missing __aeabi_* symbols at link time. */

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);

void *__aeabi_memcpy(void *dst, const void *src, size_t n)
{
    return memcpy(dst, src, n);
}

void *__aeabi_memcpy4(void *dst, const void *src, size_t n)
{
    return memcpy(dst, src, n);
}

void *__aeabi_memcpy8(void *dst, const void *src, size_t n)
{
    return memcpy(dst, src, n);
}

void *__aeabi_memset(void *dst, size_t n, int c)
{
    /* ARM EABI swaps the last two arguments relative to memset. */
    return memset(dst, c, n);
}

void *__aeabi_memmove(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

/* The zeroing and 4/8-aligned variants, plus the unaligned-access helpers.
 * Clang emits them at -Oz even with -fno-builtin (aggregate zeroing and
 * packed reads), so they have to exist even though -O2 never needed them. */
void *__aeabi_memclr(void *dst, size_t n)  { return memset(dst, 0, n); }
void *__aeabi_memclr4(void *dst, size_t n) { return memset(dst, 0, n); }
void *__aeabi_memclr8(void *dst, size_t n) { return memset(dst, 0, n); }
void *__aeabi_memset4(void *dst, size_t n, int c) { return memset(dst, c, n); }
void *__aeabi_memset8(void *dst, size_t n, int c) { return memset(dst, c, n); }
void *__aeabi_memmove4(void *dst, const void *src, size_t n) { return memmove(dst, src, n); }
void *__aeabi_memmove8(void *dst, const void *src, size_t n) { return memmove(dst, src, n); }

/* Cortex-M0+ has no unaligned load/store: byte accesses, volatile so the
 * optimiser cannot merge them back into a word access. */
uint32_t __aeabi_uread4(const void *addr)
{
    const volatile uint8_t *p = addr;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void __aeabi_uwrite4(void *addr, uint32_t v)
{
    volatile uint8_t *p = addr;

    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t __aeabi_uidiv(uint32_t n, uint32_t d)
{
    return __udivsi3(n, d);
}

int __aeabi_idiv(int n, int d)
{
    int neg = (n < 0) != (d < 0);
    uint32_t un = (uint32_t)(n < 0 ? -n : n);
    uint32_t ud = (uint32_t)(d < 0 ? -d : d);
    uint32_t q = __udivsi3(un, ud);
    return neg ? -(int)q : (int)q;
}

/* 64-bit helpers the ARM EABI names differently from the libgcc-style
 * symbols above: on AArch32 clang emits calls to __aeabi_llsl/llsr/lasr
 * for every 64-bit shift and __aeabi_lmul for every 64x64 multiply (the
 * M0+ has neither UMULL nor a wider-than-32-bit shift, so nothing is
 * inlined; rv32 and x86-32 reach the __*di3 forms instead).  They wrap
 * the half-word implementations above, which contain no 64-bit shifts or
 * multiplies of their own and therefore cannot recurse. */
uint64_t __aeabi_llsl(uint64_t v, int n)      { return __ashldi3(v, n); }
uint64_t __aeabi_llsr(uint64_t v, int n)      { return __lshrdi3(v, n); }
int64_t  __aeabi_lasr(int64_t v, int n)       { return __ashrdi3(v, n); }
uint64_t __aeabi_lmul(uint64_t a, uint64_t b) { return __muldi3(a, b); }

/* littlefs asks for __builtin_clz when walking its allocation bitmap; the
 * M0+ has no CLZ instruction, so clang lowers it to this call. */
int __clzsi2(uint32_t x)
{
    int n = 0;

    if (x == 0)
        return 32;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

/* Shared body for the "divmod" entry points.  It cannot be written as a C
 * function returning a struct: the ARM EABI returns the quotient and the
 * remainder in separate register pairs (r0:r1 and r2:r3), while AAPCS
 * would hand a 16-byte struct back through a hidden pointer.  That mismatch
 * is silent - every 64-bit division in the kernel, including the uptime
 * calculation, came out as zero.  These shims marshal the arguments by
 * hand and call a plain C helper. */
void tos_udivmod64(uint32_t nlo, uint32_t nhi, uint32_t dlo, uint32_t dhi,
                   uint32_t *out)
{
    uint64_t n = ((uint64_t)nhi << 32) | nlo;
    uint64_t d = ((uint64_t)dhi << 32) | dlo;
    uint64_t q = __udivdi3(n, d);
    uint64_t r = __umoddi3(n, d);

    out[0] = (uint32_t)q;
    out[1] = (uint32_t)(q >> 32);
    out[2] = (uint32_t)r;
    out[3] = (uint32_t)(r >> 32);
}

__asm__(
    "    .syntax unified\n"
    "    .thumb\n"
    "    .text\n"
    /* No .cpu directive: the command line -mcpu (cortex-m3 for mps2,
     * cortex-m0plus for rp2040) decides, and every instruction below is
     * deliberately kept in the ARMv6-M subset so one body serves both.
     * The two Thumb-2 forms the M3 would allow - three-register MUL and
     * SUB with high registers - are spelled with movs/muls instead. */

    /* r0:r1 = n, r2:r3 = d  ->  r0:r1 = n/d, r2:r3 = n%d */
    "    .p2align 1\n"
    "    .globl  __aeabi_uldivmod\n"
    "    .type   __aeabi_uldivmod, %function\n"
    "    .thumb_func\n"
    "__aeabi_uldivmod:\n"
    "    push    {r4, lr}\n"
    "    sub     sp, #16\n"          /* q(8) + r(8)                     */
    "    mov     r4, sp\n"
    "    sub     sp, #8\n"           /* 5th argument slot, keeps SP %8  */
    "    str     r4, [sp]\n"
    "    bl      tos_udivmod64\n"
    "    add     sp, #8\n"
    "    ldr     r0, [r4, #0]\n"
    "    ldr     r1, [r4, #4]\n"
    "    ldr     r2, [r4, #8]\n"
    "    ldr     r3, [r4, #12]\n"
    "    add     sp, #16\n"
    "    pop     {r4, pc}\n"
    "    .size   __aeabi_uldivmod, . - __aeabi_uldivmod\n"

    /* r0 = n, r1 = d  ->  r0 = n/d, r1 = n%d */
    "    .p2align 1\n"
    "    .globl  __aeabi_uidivmod\n"
    "    .type   __aeabi_uidivmod, %function\n"
    "    .thumb_func\n"
    "__aeabi_uidivmod:\n"
    "    push    {r4, lr}\n"
    "    sub     sp, #16\n"
    "    mov     r4, sp\n"
    "    mov     r2, r1\n"
    "    movs    r1, #0\n"
    "    movs    r3, #0\n"
    "    sub     sp, #8\n"
    "    str     r4, [sp]\n"
    "    bl      tos_udivmod64\n"
    "    add     sp, #8\n"
    "    ldr     r0, [r4, #0]\n"
    "    ldr     r1, [r4, #8]\n"
    "    add     sp, #16\n"
    "    pop     {r4, pc}\n"
    "    .size   __aeabi_uidivmod, . - __aeabi_uidivmod\n"

    /* r0 = n, r1 = d  ->  r0 = n/d, r1 = n%d (signed) */
    "    .p2align 1\n"
    "    .globl  __aeabi_idivmod\n"
    "    .type   __aeabi_idivmod, %function\n"
    "    .thumb_func\n"
    "__aeabi_idivmod:\n"
    "    push    {r4, r5, lr}\n"
    "    mov     r4, r0\n"           /* n                               */
    "    mov     r5, r1\n"           /* d                               */
    "    bl      __aeabi_idiv\n"
    "    mov     r2, r0\n"           /* q                               */
    "    movs    r3, r2\n"
    "    muls    r3, r5\n"           /* q*d (MULS: r3 = r3 * r5)        */
    "    subs    r1, r4, r3\n"       /* n - q*d                         */
    "    mov     r0, r2\n"
    "    pop     {r4, r5, pc}\n"
    "    .size   __aeabi_idivmod, . - __aeabi_idivmod\n");

#endif

