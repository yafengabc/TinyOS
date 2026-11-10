#ifndef TINYOS_SOFTFLOAT32_H
#define TINYOS_SOFTFLOAT32_H

#include "types.h"

/* binary32 arithmetic for targets without an FPU.
 *
 * The names are the ones clang/LLVM and libgcc emit for float operators, so a
 * user program can use plain `float` on RV32IMC / Cortex-M0+ and link these.
 * Everything is computed from the IEEE-754 bit pattern with 32-bit integer
 * arithmetic; no f32/double operation appears in the implementation itself.
 *
 * goc's runtime (src/goclib/softfloat.c) implements the double versions and
 * turns every float operation into a double one.  This is the native 32-bit
 * counterpart: one 27-bit working significand (24 bits + guard/round/sticky)
 * that fits in a register. */

float __addsf3(float a, float b);
float __subsf3(float a, float b);
float __mulsf3(float a, float b);
float __divsf3(float a, float b);
float __negsf2(float a);

int __eqsf2(float a, float b);
int __nesf2(float a, float b);
int __ltsf2(float a, float b);
int __lesf2(float a, float b);
int __gtsf2(float a, float b);
int __gesf2(float a, float b);
int __unordsf2(float a, float b);

int __fixsfsi(float a);
unsigned int __fixunssfsi(float a);
float __floatsisf(int a);
float __floatunsisf(unsigned int a);

/* ARM EABI spellings, for the Cortex-M soft-float ABI. */
float __aeabi_fadd(float a, float b);
float __aeabi_fsub(float a, float b);
float __aeabi_fmul(float a, float b);
float __aeabi_fdiv(float a, float b);
float __aeabi_frsub(float a, float b);
int __aeabi_fcmplt(float a, float b);
int __aeabi_fcmple(float a, float b);
int __aeabi_fcmpeq(float a, float b);
int __aeabi_fcmpgt(float a, float b);
int __aeabi_fcmpge(float a, float b);
int __aeabi_fcmpun(float a, float b);
float __aeabi_i2f(int a);
float __aeabi_ui2f(unsigned int a);
int __aeabi_f2iz(float a);
unsigned int __aeabi_f2uiz(float a);

/* Format/print a float without touching printf.  `float_to_str` writes at most
 * cap-1 characters plus NUL and returns the length it wanted.  `print_float`
 * writes to stdout with four decimals and no trailing newline.  Values of
 * 2^31 and above print as "big". */
int  float_to_str(char *out, int cap, float v, int decimals);
int  print_float_n(float v, int decimals);
int  print_float(float v);

#endif /* TINYOS_SOFTFLOAT32_H */
