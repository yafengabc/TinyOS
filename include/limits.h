#ifndef TINYOS_STD_LIMITS_H
#define TINYOS_STD_LIMITS_H

/* Minimal freestanding limits.h.
 *
 * The TinyOS user include set has no limits.h, but vendored sources (the
 * Pawn AMX core, for one) include it for INT_MAX.  Keep this to the
 * constants those sources actually use; it is not a full C library header.
 */

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255

#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535

#define INT_MIN    (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295U

#define LONG_MIN   (-2147483647L - 1L)
#define LONG_MAX   2147483647L
#define ULONG_MAX  4294967295UL

#define PATH_MAX   256

#endif
