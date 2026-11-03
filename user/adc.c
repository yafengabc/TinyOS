#include "lib.h"

/* adc - sample the analog inputs.
 *
 *   adc                describe the converter and sample every channel once
 *   adc <chan>         sample one channel
 *   adc <chan> <n>     sample it n times, 20 ms apart
 *
 * Raw counts are what the hardware reports; millivolts are counts scaled by
 * the reference the board reports, so the same program reads correctly on a
 * 3.3 V part and a 1.8 V one. */

static int to_mv(int raw, int vref, int bits)
{
    int full = (1 << bits) - 1;

    if (full <= 0)
        return 0;
    /* raw * vref can overflow 32 bits for large vref, so divide first. */
    return (raw / full) * vref + ((raw % full) * vref) / full;
}

static void sample(int chan, int vref, int bits)
{
    int raw = adc_read(chan);

    if (raw < 0) {
        printf("adc %d: conversion failed\n", chan);
        return;
    }
    printf("adc %d: %4d  = %4d mV\n", chan, raw, to_mv(raw, vref, bits));
}

int main(int argc, char *argv[])
{
    int n = adc_count();
    int vref, bits;

    if (n <= 0) {
        puts("adc: this board has no ADC wired up for user space\n");
        return 1;
    }

    vref = adc_vref_mv();
    bits = adc_bits();

    if (argc < 2) {
        printf("adc: %d channels, %d bits, %d mV reference\n", n, bits, vref);
        for (int i = 0; i < n; i++)
            sample(i, vref, bits);
        return 0;
    }

    {
        int chan = atoi(argv[1]);
        int times = argc > 2 ? atoi(argv[2]) : 1;

        if (chan < 0 || chan >= n) {
            printf("adc: channel %d does not exist (0..%d)\n", chan, n - 1);
            return 1;
        }
        if (times < 1)
            times = 1;
        if (times > 1000)
            times = 1000;

        for (int i = 0; i < times; i++) {
            sample(chan, vref, bits);
            if (i + 1 < times)
                sleep(20);
        }
        return 0;
    }
}
