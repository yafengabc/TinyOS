#include "lib.h"

/* i2c - scan or talk to a device on the board's I2C bus(es).
 *
 *   i2c                                   describe the buses
 *   i2c scan <bus> [hz]                   probe 0x08..0x77
 *   i2c read <bus> <addr> <reg> <len>     write reg, then read len bytes
 *   i2c write <bus> <addr> <reg> <byte>... write reg followed by hex bytes
 *
 * The pin map is board policy and `cat /proc/dev` lists it.  Scan reads one
 * byte from each address because a zero-length transaction is not portable;
 * that is still a normal address-probe and an absent device aborts cleanly.
 */

static void usage(void)
{
    puts("usage: i2c scan <bus> [hz] | i2c read <bus> <addr> <reg> <len> | "
         "i2c write <bus> <addr> <reg> <hex>...\n");
}

static int hexval(const char *s)
{
    int v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else
            return -1;
        v = (v << 4) | d;
        if (v > 0xff)
            return -1;
    }
    return v;
}

static void hexdump(const unsigned char *p, int n)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < n; i++) {
        if (i)
            putchar(' ');
        putchar(hex[p[i] >> 4]);
        putchar(hex[p[i] & 15]);
    }
    putchar('\n');
}

int main(int argc, char *argv[])
{
    int n = i2c_count();

    if (n <= 0) {
        puts("i2c: this board has no I2C master for user space\n");
        return 1;
    }

    if (argc < 2) {
        printf("i2c: %d bus(es); 'cat /proc/dev' lists sda/scl\n", n);
        usage();
        return 0;
    }

    if (!strcmp(argv[1], "scan")) {
        int bus, hz = 100000, found = 0;

        if (argc < 3) {
            usage();
            return 1;
        }
        bus = atoi(argv[2]);
        if (argc > 3)
            hz = atoi(argv[3]);

        printf("i2c: scanning bus %d at %d Hz\n", bus, hz);
        for (int addr = 0x08; addr <= 0x77; addr++) {
            unsigned char b;

            if (i2c_xfer(bus, addr, hz, 0, 0, &b, 1) >= 0) {
                printf("  0x%02x\n", addr);
                found++;
            }
        }
        printf("i2c: %d device(s)\n", found);
        return 0;
    }

    if (!strcmp(argv[1], "read")) {
        int bus, addr, reg, len;
        unsigned char out[64];

        if (argc < 6) {
            usage();
            return 1;
        }
        bus  = atoi(argv[2]);
        addr = hexval(argv[3]);
        reg  = hexval(argv[4]);
        len  = atoi(argv[5]);
        if (addr < 0 || reg < 0 || len <= 0 || len > (int)sizeof(out)) {
            puts("i2c: bad address/register/length\n");
            return 1;
        }
        {
            unsigned char r = (unsigned char)reg;
            if (i2c_xfer(bus, addr, 100000, &r, 1, out, len) < 0) {
                printf("i2c: read from 0x%02x failed\n", addr);
                return 1;
            }
        }
        hexdump(out, len);
        return 0;
    }

    if (!strcmp(argv[1], "write")) {
        unsigned char buf[64];
        int bus, addr, reg, len = 0;

        if (argc < 5) {
            usage();
            return 1;
        }
        bus  = atoi(argv[2]);
        addr = hexval(argv[3]);
        reg  = hexval(argv[4]);
        if (addr < 0 || reg < 0) {
            puts("i2c: bad address/register\n");
            return 1;
        }
        buf[len++] = (unsigned char)reg;
        for (int i = 5; i < argc && len < (int)sizeof(buf); i++) {
            int v = hexval(argv[i]);
            if (v < 0) {
                printf("i2c: bad hex byte %s\n", argv[i]);
                return 1;
            }
            buf[len++] = (unsigned char)v;
        }
        if (i2c_xfer(bus, addr, 100000, buf, len, 0, 0) < 0) {
            printf("i2c: write to 0x%02x failed\n", addr);
            return 1;
        }
        printf("i2c: wrote %d byte(s) to 0x%02x\n", len, addr);
        return 0;
    }

    usage();
    return 1;
}
