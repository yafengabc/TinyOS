#ifndef TINYOS_FIX16_H
#define TINYOS_FIX16_H

#include "types.h"

/* Q16.16 fixed point, the arithmetic counterpart to softfloat32.
 *
 * Range +/-32767.99998, resolution 1/65536.  It exists to answer the size
 * question: what does a float cost on a board with no FPU, and what does the
 * same program cost if the values are integers scaled by 65536?
 *
 * Division is a 32-bit restoring division (a 16-bit integer part plus a
 * 16-bit fraction), so the whole library stays free of 64-bit helpers.
 * Printing is integer-only and never calls the float library. */

typedef int32_t fix16_t;

#define FIX16_SHIFT 16
#define FIX16_ONE   (1 << FIX16_SHIFT)
#define FIX16_MAX   ((fix16_t)0x7FFFFFFF)
#define FIX16_MIN   ((fix16_t)(int32_t)0x80000000)

fix16_t fix16_from_int(int v);
int     fix16_to_int(fix16_t v);        /* truncates toward zero */
fix16_t fix16_add(fix16_t a, fix16_t b);
fix16_t fix16_sub(fix16_t a, fix16_t b);
fix16_t fix16_mul(fix16_t a, fix16_t b);
fix16_t fix16_div(fix16_t a, fix16_t b);
fix16_t fix16_abs(fix16_t v);

/* Same interface as float_to_str/printfloat, up to four decimals. */
int  fix16_to_str(char *out, int cap, fix16_t v, int decimals);
int  printfix16_n(fix16_t v, int decimals);
int  printfix16(fix16_t v);

#endif /* TINYOS_FIX16_H */
