/*
 * TinyOS port of CoreMark 1.0 (EEMBC).
 *
 * Kept deliberately thin: the benchmark core files are the upstream ones,
 * and this header only describes the machine to them.  CORE_TICKS is
 * milliseconds from the TinyOS clock, HAS_FLOAT is 0 because the RV32
 * user programs link without soft-float helpers, and the data block is
 * static so a 16 KiB window does not have to carry a second 2 KiB stack
 * array.  Reporting lives in core_main.c / core_portme.c; iterate() and
 * the CRCs are untouched.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include "types.h"

/* No floating point: the user ABI is ilp32 and no compiler-rt is linked. */
#ifndef HAS_FLOAT
#define HAS_FLOAT 0
#endif
#ifndef HAS_TIME_H
#define HAS_TIME_H 0
#endif
#ifndef USE_CLOCK
#define USE_CLOCK 0
#endif
#ifndef HAS_STDIO
#define HAS_STDIO 0
#endif
#ifndef HAS_PRINTF
#define HAS_PRINTF 0
#endif

#ifndef TOTAL_DATA_SIZE
#define TOTAL_DATA_SIZE 2000        /* 2K performance run */
#endif
#ifndef ITERATIONS
#define ITERATIONS 0                /* 0 = auto-calibrate to ~10 s */
#endif

/* Strings the official report prints. */
#ifndef COMPILER_VERSION
#define COMPILER_VERSION __VERSION__
#endif
#ifndef COMPILER_FLAGS
#if defined(__riscv)
#define COMPILER_FLAGS "-O2 rv32imac -mabi=ilp32"
#elif defined(__arm__)
#define COMPILER_FLAGS "-O2 thumbv8m.main -mcpu=cortex-m33"
#else
#define COMPILER_FLAGS "-O2"
#endif
#endif
#define MEM_LOCATION "STATIC"

/* Data types: ee_ptr_int must hold a pointer. */
typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef ee_u32         ee_ptr_int;
typedef size_t         ee_size_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x) - 1) & ~3))

/* Timing: one tick is one millisecond (see core_portme.c). */
#define CORETIMETYPE ee_u32
typedef ee_u32 CORE_TICKS;

#define SEED_METHOD   SEED_VOLATILE
#define MEM_METHOD    MEM_STATIC
#define MULTITHREAD   1
#define MAIN_HAS_NOARGC   0
#define MAIN_HAS_NORETURN 0

extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S
{
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

int ee_printf(const char *fmt, ...);

#endif /* CORE_PORTME_H */
