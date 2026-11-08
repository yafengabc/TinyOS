#ifndef TINYOS_STDARG_SHIM_H
#define TINYOS_STDARG_SHIM_H

/* Minimal freestanding stdarg.h for vendored code (CoreMark's ee_printf
 * includes it).  The tree builds with -nostdinc, so the compiler's own
 * header is not on the path; these builtins are what the kernel uses
 * directly. */

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(dst, src)  __builtin_va_copy(dst, src)

#endif
