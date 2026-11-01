#ifndef TINYOS_ARCH_USYS_H
#define TINYOS_ARCH_USYS_H

/* User-mode system call entry for ARM (Cortex-M): svc #0 with the number in
 * r0 and arguments in r1/r2/r3, result in r0.  Included by user/syscall.c
 * so the wrappers stay architecture independent. */

static inline int tos_sys0(int n)
{
    register int r0 __asm__("r0") = n;
    __asm__ volatile("svc #0" : "+r"(r0) : : "memory");
    return r0;
}

static inline int tos_sys1(int n, int a)
{
    register int r0 __asm__("r0") = n;
    register int r1 __asm__("r1") = a;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static inline int tos_sys2(int n, int a, int b)
{
    register int r0 __asm__("r0") = n;
    register int r1 __asm__("r1") = a;
    register int r2 __asm__("r2") = b;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
    return r0;
}

static inline int tos_sys3(int n, int a, int b, int c)
{
    register int r0 __asm__("r0") = n;
    register int r1 __asm__("r1") = a;
    register int r2 __asm__("r2") = b;
    register int r3 __asm__("r3") = c;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3)
                     : "memory");
    return r0;
}

#endif
