#include "lib.h"

/* uart - talk to the extra serial port (UART1 on the Pico).
 *
 *   uart                 describe the ports
 *   uart send <text...>  transmit the words, separated by spaces
 *   uart recv [ms]       poll for input for ms milliseconds (default 1000)
 *
 * Reads are deliberately non-blocking at the syscall layer.  A blocking
 * read would have to park a task on a UART FIFO, and the poll loop here is
 * enough to make a loopback test or a small protocol usable from shell.
 */

static void usage(void)
{
    puts("usage: uart send <text...> | uart recv [ms]\n");
}

int main(int argc, char *argv[])
{
    int n = uart_count();

    if (n <= 0) {
        puts("uart: this board has no extra UART for user space\n");
        return 1;
    }

    if (argc < 2) {
        printf("uart: %d port(s), UART1 TX=GP4 RX=GP5 115200 8N1\n", n);
        usage();
        return 0;
    }

    if (!strcmp(argv[1], "send")) {
        if (argc < 3) {
            usage();
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                uart_write(0, " ", 1);
            if (uart_write(0, argv[i], (int)strlen(argv[i])) < 0) {
                puts("uart: write failed\n");
                return 1;
            }
        }
        uart_write(0, "\r\n", 2);
        return 0;
    }

    if (!strcmp(argv[1], "recv")) {
        char buf[64];
        int ms = argc > 2 ? atoi(argv[2]) : 1000;
        int total = 0;

        if (ms < 0)
            ms = 0;
        for (int waited = 0; waited < ms; waited += 10) {
            int k = uart_read(0, buf, sizeof(buf));

            if (k > 0) {
                write_all(1, buf, k);
                total += k;
            }
            sleep(10);
        }
        printf("uart: %d byte(s)\n", total);
        return 0;
    }

    usage();
    return 1;
}
