#include "lib.h"

/* spi - drive the board's general-purpose SPI master.
 *
 *   spi                                      describe the buses
 *   spi loop <bus> <cs> [hz]                 external MOSI->MISO check
 *   spi loopback <bus> [hz]                  internal loopback check
 *   spi send <bus> <cs> <hz> <text...>       send text, print received bytes
 *
 * <cs> is a GPIO number, or -1 when the caller owns the chip select.  The
 * pin map differs per board (RP2040 has SPI0/SPI1; the C3 has one SPI2) and
 * is listed by `cat /proc/dev`.  For `spi loop`, jumper the bus's MOSI pin
 * to its MISO pin; `spi loopback` needs a controller with an internal
 * loopback bit (the RP2040's PL022 has one, the C3 does not).
 */

static void usage(void)
{
    puts("usage: spi loop <bus> <cs> [hz] | spi loopback <bus> [hz] | "
         "spi send <bus> <cs> <hz> <text...>\n");
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
    int n = spi_count();

    if (n <= 0) {
        puts("spi: this board has no SPI master for user space\n");
        return 1;
    }

    if (argc < 2) {
        printf("spi: %d bus(es); 'cat /proc/dev' lists sck/mosi/miso\n", n);
        usage();
        return 0;
    }

    if (!strcmp(argv[1], "loop")) {
        unsigned char tx[4] = { 0x55, 0xAA, 0x00, 0xFF };
        unsigned char rx[4];
        int bus, cs, hz = 1000000;

        if (argc < 4) {
            usage();
            return 1;
        }
        bus = atoi(argv[2]);
        cs  = atoi(argv[3]);
        if (argc > 4)
            hz = atoi(argv[4]);

        if (spi_xfer(bus, cs, hz, 0, tx, rx, 4) < 0) {
            printf("spi: transfer failed on bus %d\n", bus);
            return 1;
        }
        printf("spi: sent ");
        hexdump(tx, 4);
        printf("spi: recv ");
        hexdump(rx, 4);
        for (int i = 0; i < 4; i++) {
            if (rx[i] != tx[i]) {
                puts("spi: loopback mismatch (check TX->RX jumper)\n");
                return 1;
            }
        }
        puts("spi: loopback ok\n");
        return 0;
    }

    if (!strcmp(argv[1], "loopback")) {
        unsigned char tx[4] = { 0x55, 0xAA, 0x00, 0xFF };
        unsigned char rx[4];
        int bus, hz = 1000000;

        if (argc < 3) {
            usage();
            return 1;
        }
        bus = atoi(argv[2]);
        if (argc > 3)
            hz = atoi(argv[3]);

        if (spi_xfer(bus, -1, hz, 4, tx, rx, 4) < 0) {
            printf("spi: internal loopback failed on bus %d\n", bus);
            return 1;
        }
        printf("spi: sent ");
        hexdump(tx, 4);
        printf("spi: recv ");
        hexdump(rx, 4);
        for (int i = 0; i < 4; i++) {
            if (rx[i] != tx[i]) {
                puts("spi: internal loopback mismatch\n");
                return 1;
            }
        }
        puts("spi: internal loopback ok\n");
        return 0;
    }

    if (!strcmp(argv[1], "send")) {
        unsigned char tx[128], rx[128];
        int bus, cs, hz, len = 0;

        if (argc < 6) {
            usage();
            return 1;
        }
        bus = atoi(argv[2]);
        cs  = atoi(argv[3]);
        hz  = atoi(argv[4]);

        for (int i = 5; i < argc; i++) {
            if (i > 5 && len < (int)sizeof(tx))
                tx[len++] = ' ';
            for (const char *s = argv[i]; *s && len < (int)sizeof(tx); s++)
                tx[len++] = (unsigned char)*s;
        }

        int got = spi_xfer(bus, cs, hz, 0, tx, rx, len);
        if (got < 0) {
            printf("spi: transfer failed on bus %d\n", bus);
            return 1;
        }
        printf("spi: sent %d byte(s), recv:\n", got);
        hexdump(rx, got);
        return 0;
    }

    usage();
    return 1;
}
