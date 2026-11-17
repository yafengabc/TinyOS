/* Host-side stand-in for a panel lower: it prints what the shared ST7789
 * protocol tells the bus to do, so lcd_proto.h can be compared against a
 * golden sequence on a machine with no screen and no SPI.
 *
 * Built by internal/build/lcdprotomock_test.go, which copies this file next to
 * lcd_proto.h and a stub types.h.  Output is a single line of space-separated
 * tokens:
 *
 *   Cxx          command byte
 *   Dhhhh..      data payload, hex
 *   Wnn          delay in milliseconds
 *
 * Keep it free of anything a hosted compiler cannot do: no kernel headers, no
 * MMIO, no stdio beyond printf.
 */
#include <stdio.h>
#include <stdlib.h>

#include "lcd_proto.h"

static void mock_cmd(uint8_t c)
{
    printf("C%02x ", c);
}

static void mock_data(const uint8_t *d, uint32_t n)
{
    printf("D");
    for (uint32_t i = 0; i < n; i++)
        printf("%02x", d[i]);
    printf(" ");
}

static void mock_delay(uint32_t ms)
{
    printf("W%u ", ms);
}

static const struct lcd_proto_ops ops = { mock_cmd, mock_data, mock_delay };

int main(int argc, char **argv)
{
    int mode = argc > 1 ? atoi(argv[1]) : 0;

    if (mode == 0) {
        lcd_proto_panel_init(&ops);
    } else {
        /* mode 1: the address window, including the degenerate cases. */
        lcd_proto_window(&ops, 0, 0, 240, 135, 40, 53);
        lcd_proto_window(&ops, 0, 0, 240, 135, 40, 53);
        lcd_proto_window(&ops, 6, 8, 6, 8, 40, 53);
        lcd_proto_window(&ops, 0, 0, 0, 135, 40, 53);
        lcd_proto_window(&ops, 0, 0, 240, -1, 40, 53);
    }
    printf("\n");
    return 0;
}
