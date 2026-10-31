#ifndef TINYOS_ARCH_USYS_H
#define TINYOS_ARCH_USYS_H

/* User-mode system call entry for RV32: ecall, a7 = number, a0/a1/a2 =
 * arguments, a0 = result.  Included by user/syscall.c so the wrappers stay
 * architecture independent. */

static inline int tos_sys0(int n)
{
    register int a7 __asm__("a7") = n;
    register int a0 __asm__("a0");
    __asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int tos_sys1(int n, int a)
{
    register int a7 __asm__("a7") = n;
    register int a0 __asm__("a0") = a;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int tos_sys2(int n, int a, int b)
{
    register int a7 __asm__("a7") = n;
    register int a0 __asm__("a0") = a;
    register int a1 __asm__("a1") = b;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1) : "memory");
    return a0;
}

static inline int tos_sys3(int n, int a, int b, int c)
{
    register int a7 __asm__("a7") = n;
    register int a0 __asm__("a0") = a;
    register int a1 __asm__("a1") = b;
    register int a2 __asm__("a2") = c;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}

#endif
