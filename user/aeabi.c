#include "types.h"

#if defined(__arm__)

/* ARM EABI division helpers for user programs.
 *
 * The Cortex-M0+ (RP2040) has no hardware divide, so every / and % that the
 * compiler cannot fold into a shift becomes a call to __aeabi_uidiv and
 * friends.  The Cortex-M3 (qemu-mps2) has UDIV/SDIV, so nothing references
 * this file there and --gc-sections drops it.  User programs link only
 * crt0 + lib + syscall, so unlike the kernel they cannot borrow
 * kernel/libgcc.c; the helpers here are self-contained.
 *
 * The divmod entry points return the quotient and the remainder in separate
 * registers (r0 and r1), which a C function cannot express - AAPCS would
 * hand a two-int struct back through a hidden pointer.  Like the kernel,
 * the shims marshal registers by hand and call a plain C helper. */

static uint32_t tos_udiv32(uint32_t n, uint32_t d)
{
    uint32_t q = 0, r = 0;
    int i;

    if (d == 0)
        return 0;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (1u << i);
        }
    }
    return q;
}

uint32_t __aeabi_uidiv(uint32_t n, uint32_t d)
{
    return tos_udiv32(n, d);
}

int __aeabi_idiv(int n, int d)
{
    int neg = (n < 0) != (d < 0);
    uint32_t un = (uint32_t)(n < 0 ? -n : n);
    uint32_t ud = (uint32_t)(d < 0 ? -d : d);
    uint32_t q = tos_udiv32(un, ud);
    return neg ? -(int)q : (int)q;
}

__asm__(
    "    .syntax unified\n"
    "    .thumb\n"
    "    .text\n"
    /* Same ARMv6-M-only subset as kernel/libgcc.c: no .cpu directive (the
     * build's -mcpu decides), movs/muls instead of three-register MUL. */

    /* r0 = n, r1 = d  ->  r0 = n/d, r1 = n%d */
    "    .p2align 1\n"
    "    .globl  __aeabi_uidivmod\n"
    "    .type   __aeabi_uidivmod, %function\n"
    "    .thumb_func\n"
    "__aeabi_uidivmod:\n"
    "    push    {r4, r5, lr}\n"
    "    mov     r4, r0\n"           /* n                               */
    "    mov     r5, r1\n"           /* d                               */
    "    bl      __aeabi_uidiv\n"
    "    mov     r2, r0\n"           /* q                               */
    "    movs    r3, r2\n"
    "    muls    r3, r5\n"           /* q*d                             */
    "    subs    r1, r4, r3\n"       /* n - q*d                         */
    "    mov     r0, r2\n"
    "    pop     {r4, r5, pc}\n"
    "    .size   __aeabi_uidivmod, . - __aeabi_uidivmod\n"

    /* r0 = n, r1 = d  ->  r0 = n/d, r1 = n%d (signed) */
    "    .p2align 1\n"
    "    .globl  __aeabi_idivmod\n"
    "    .type   __aeabi_idivmod, %function\n"
    "    .thumb_func\n"
    "__aeabi_idivmod:\n"
    "    push    {r4, r5, lr}\n"
    "    mov     r4, r0\n"
    "    mov     r5, r1\n"
    "    bl      __aeabi_idiv\n"
    "    mov     r2, r0\n"           /* q                               */
    "    movs    r3, r2\n"
    "    muls    r3, r5\n"           /* q*d                             */
    "    subs    r1, r4, r3\n"       /* n - q*d                         */
    "    mov     r0, r2\n"
    "    pop     {r4, r5, pc}\n"
    "    .size   __aeabi_idivmod, . - __aeabi_idivmod\n");

/* Cortex-M0+ has no CLZ, and clang lowers the normalisation loops in the
 * soft-float and fixed-point libraries to a call.  Binary search, five
 * steps, no table. */
uint32_t __clzsi2(uint32_t x)
{
    uint32_t n = 0;

    if (x == 0)
        return 32;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8;  x <<= 8;  }
    if (!(x & 0xF0000000u)) { n += 4;  x <<= 4;  }
    if (!(x & 0xC0000000u)) { n += 2;  x <<= 2;  }
    if (!(x & 0x80000000u)) { n += 1; }
    return n;
}

/* Clang emits these at -Oz for aggregate zeroing, aligned copies and packed
 * reads, even with -fno-builtin.  lib.c supplies the byte loops. */
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

void *__aeabi_memmove(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

void *__aeabi_memmove4(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

void *__aeabi_memmove8(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

void *__aeabi_memclr(void *dst, size_t n)  { return memset(dst, 0, n); }
void *__aeabi_memclr4(void *dst, size_t n) { return memset(dst, 0, n); }
void *__aeabi_memclr8(void *dst, size_t n) { return memset(dst, 0, n); }
void *__aeabi_memset(void *dst, size_t n, int c)  { return memset(dst, c, n); }
void *__aeabi_memset4(void *dst, size_t n, int c) { return memset(dst, c, n); }
void *__aeabi_memset8(void *dst, size_t n, int c) { return memset(dst, c, n); }

/* Cortex-M0+ has no unaligned access; volatile byte loads keep LLVM from
 * merging them back into one word access. */
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

#endif /* __arm__ */
