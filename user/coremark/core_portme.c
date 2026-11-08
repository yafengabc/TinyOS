/*
 * TinyOS port layer for CoreMark 1.0 (EEMBC).
 *
 * Only the things CoreMark cannot know: the seeds for a 2K performance run,
 * a millisecond clock, and the static/portable allocation hooks.  All
 * output goes through ee_printf.c -> uart_send_char -> putchar, so it is
 * the same console path every other user program uses.
 *
 *   coremark            auto-calibrate to a ~10 s run
 *   coremark <iters>    fixed iteration count (useful on a slow target)
 */
#include "coremark.h"
#include "lib.h"

/* 2K performance run seeds: 0, 0, 0x66, auto iterations, no extra seeds.
 * The resulting seedcrc is 0xe9f5. */
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

static CORE_TICKS start_val;
static CORE_TICKS stop_val;

void start_time(void)
{
    start_val = clock_ms();
}

void stop_time(void)
{
    stop_val = clock_ms();
}

CORE_TICKS get_time(void)
{
    return stop_val - start_val;        /* milliseconds */
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    /* HAS_FLOAT is 0, so this is the integer-seconds view the official
     * report uses.  The [tos] line in core_main.c uses the raw millisecond
     * ticks for a score with real resolution. */
    return (secs_ret)(ticks / 1000u);
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    p->portable_id = 1;

    if (*argc > 1)
        seed4_volatile = atoi(argv[1]);

    printf("\nCoreMark 1.0 - TinyOS (%s)\n",
           seed4_volatile ? "fixed iterations" : "auto-calibrating");
}

void portable_fini(core_portable *p)
{
    (void)p;
}

/* MEM_STATIC and SEED_VOLATILE do not use these, but the header declares
 * them and a different build (MEM_MALLOC) would. */
void *portable_malloc(ee_size_t size)
{
    (void)size;
    return NULL;
}

void portable_free(void *p)
{
    (void)p;
}
