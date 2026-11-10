#include "softfloat32.h"

/* ARM EABI spellings of the float runtime.
 *
 * Cortex-M code compiled with -mfloat-abi=soft names the __aeabi_f* entry
 * points instead of the compiler-rt __*sf* ones.  The bodies just forward, so
 * this file works with either backend (softfloat32.c or softfix.c) and the
 * unused wrappers fall to --gc-sections. */

float __aeabi_fadd(float a, float b) { return __addsf3(a, b); }
float __aeabi_fsub(float a, float b) { return __subsf3(a, b); }
float __aeabi_frsub(float a, float b) { return __subsf3(b, a); }
float __aeabi_fmul(float a, float b) { return __mulsf3(a, b); }
float __aeabi_fdiv(float a, float b) { return __divsf3(a, b); }
int __aeabi_fcmplt(float a, float b) { return __ltsf2(a, b) < 0; }
int __aeabi_fcmple(float a, float b) { return __lesf2(a, b) < 0; }
int __aeabi_fcmpeq(float a, float b) { return __eqsf2(a, b) == 0; }
int __aeabi_fcmpgt(float a, float b) { return __gtsf2(a, b) > 0; }
int __aeabi_fcmpge(float a, float b) { return __gesf2(a, b) > 0; }
int __aeabi_fcmpun(float a, float b) { return __unordsf2(a, b); }
float __aeabi_i2f(int a) { return __floatsisf(a); }
float __aeabi_ui2f(unsigned int a) { return __floatunsisf(a); }
int __aeabi_f2iz(float a) { return __fixsfsi(a); }
unsigned int __aeabi_f2uiz(float a) { return __fixunssfsi(a); }
