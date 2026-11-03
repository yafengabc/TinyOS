#include "lib.h"

/* gpio - drive and read the board's pins.
 *
 *   gpio                      list what the board offers
 *   gpio <pin>                read the level on a pin
 *   gpio <pin> 0|1            drive a pin (configures it as an output)
 *   gpio mode <pin> in|out|up|down
 *   gpio blink <pin> [n]      toggle a pin n times, 150 ms per half period
 *
 * A board with no GPIO answers gpio_count() with 0, which is why every path
 * starts there: "no GPIO" and "pin 5 is low" must not look the same. */

static const char *mode_name(int m)
{
    switch (m) {
    case TOS_PIN_IN:          return "in";
    case TOS_PIN_OUT:         return "out";
    case TOS_PIN_IN_PULLUP:   return "up";
    case TOS_PIN_IN_PULLDOWN: return "down";
    default:                  return "?";
    }
}

static int parse_mode(const char *s)
{
    if (!strcmp(s, "in"))   return TOS_PIN_IN;
    if (!strcmp(s, "out"))  return TOS_PIN_OUT;
    if (!strcmp(s, "up"))   return TOS_PIN_IN_PULLUP;
    if (!strcmp(s, "down")) return TOS_PIN_IN_PULLDOWN;
    return -1;
}

/* ---- the same pins through /dev -------------------------------------- */

/* "/dev/gpio7" built by hand: the user lib has no sprintf and no itoa in the
 * header (atoi is there, its inverse is not), and pulling in a formatter for
 * eight characters is not worth it. */
static void dev_path(char *buf, int pin)
{
    static const char pre[] = "/dev/gpio";
    char num[8];
    int i = 0, k = 0, j;

    while (pre[i]) {
        buf[i] = pre[i];
        i++;
    }
    if (pin == 0)
        num[k++] = '0';
    while (pin > 0) {
        num[k++] = (char)('0' + pin % 10);
        pin /= 10;
    }
    for (j = k - 1; j >= 0; j--)
        buf[i++] = num[j];
    buf[i] = '\0';
}

/* `gpio dev ...` drives the pin through /dev/gpioN instead of the C API.
 *
 * This is the shell path - open, read, write, ioctl - and it exists here so
 * the two routes can be compared.  They must agree; when they do not, one of
 * them is wrong, and that is worth finding before a script depends on it. */
static int dev_command(char *argv[], int argc)
{
    char path[24];
    char buf[16];
    int pin, fd, n;

    if (argc < 3) {
        puts("usage: gpio dev <pin>              read through /dev/gpioN\n");
        puts("       gpio dev <pin> 0|1          write through /dev/gpioN\n");
        puts("       gpio dev mode <pin> in|out|up|down\n");
        return 2;
    }

    if (!strcmp(argv[2], "mode")) {
        int m;

        if (argc < 5) {
            puts("usage: gpio dev mode <pin> in|out|up|down\n");
            return 2;
        }
        pin = atoi(argv[3]);
        m = parse_mode(argv[4]);
        if (m < 0) {
            printf("gpio: unknown mode '%s'\n", argv[4]);
            return 2;
        }
        dev_path(path, pin);
        fd = open(path);
        if (fd < 0) {
            printf("gpio: cannot open %s\n", path);
            return 1;
        }
        /* The mode is neither a byte to read nor one to write, which is
         * exactly what ioctl is for. */
        n = ioctl(fd, GPIO_OP_MODE, m);
        close(fd);
        if (n < 0) {
            printf("gpio: ioctl failed on %s\n", path);
            return 1;
        }
        printf("%s: mode %s (ioctl)\n", path, mode_name(m));
        return 0;
    }

    pin = atoi(argv[2]);
    dev_path(path, pin);
    fd = open(path);
    if (fd < 0) {
        printf("gpio: cannot open %s\n", path);
        return 1;
    }

    if (argc >= 4) {
        n = write(fd, argv[3], (int)strlen(argv[3]));
        close(fd);
        if (n < 0) {
            printf("%s: write failed\n", path);
            return 1;
        }
        printf("%s: wrote %s\n", path, argv[3]);
        return 0;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        printf("%s: read failed\n", path);
        return 1;
    }
    buf[n] = '\0';
    printf("%s: %s", path, buf);
    return 0;
}

int main(int argc, char *argv[])
{
    int n = gpio_count();

    if (n <= 0) {
        puts("gpio: this board exposes no GPIO to user space\n");
        return 1;
    }

    if (argc < 2) {
        printf("gpio: %d pins, 0..%d\n", n, n - 1);
        puts("usage: gpio <pin>              read the level\n");
        puts("       gpio <pin> 0|1          drive the pin\n");
        puts("       gpio mode <pin> in|out|up|down\n");
        puts("       gpio blink <pin> [n]    toggle it n times\n");
        puts("       gpio dev <pin> [0|1]    same pin through /dev/gpioN\n");
        return 0;
    }

    if (!strcmp(argv[1], "dev"))
        return dev_command(argv, argc);

    if (!strcmp(argv[1], "mode")) {
        int pin, m;
        if (argc < 4) {
            puts("usage: gpio mode <pin> in|out|up|down\n");
            return 2;
        }
        pin = atoi(argv[2]);
        m = parse_mode(argv[3]);
        if (m < 0) {
            printf("gpio: unknown mode '%s'\n", argv[3]);
            return 2;
        }
        if (gpio_mode(pin, m) < 0) {
            printf("gpio: cannot configure pin %d\n", pin);
            return 1;
        }
        printf("gpio %d: %s\n", pin, mode_name(m));
        return 0;
    }

    if (!strcmp(argv[1], "blink")) {
        int pin, times = 10;

        if (argc < 3) {
            puts("usage: gpio blink <pin> [times]\n");
            return 2;
        }
        pin = atoi(argv[2]);
        if (argc > 3)
            times = atoi(argv[3]);
        if (times <= 0)
            times = 1;

        if (gpio_mode(pin, TOS_PIN_OUT) < 0) {
            printf("gpio: cannot configure pin %d as an output\n", pin);
            return 1;
        }
        printf("gpio %d: blinking %d times\n", pin, times);
        for (int i = 0; i < times; i++) {
            gpio_write(pin, 1);
            sleep(150);
            gpio_write(pin, 0);
            sleep(150);
        }
        puts("gpio: done\n");
        return 0;
    }

    {
        int pin = atoi(argv[1]);

        if (pin < 0 || pin >= n) {
            printf("gpio: pin %d is not on this board (0..%d)\n", pin, n - 1);
            return 1;
        }

        if (argc == 2) {
            int v = gpio_read(pin);
            if (v < 0) {
                printf("gpio %d: cannot read (configure it with 'gpio mode %d in')\n",
                       pin, pin);
                return 1;
            }
            printf("gpio %d: %d\n", pin, v);
            return 0;
        }

        {
            int val = atoi(argv[2]) ? 1 : 0;
            if (gpio_mode(pin, TOS_PIN_OUT) < 0 ||
                gpio_write(pin, val) < 0) {
                printf("gpio: cannot drive pin %d\n", pin);
                return 1;
            }
            /* Read it back: the pad reports what it is driving, so this
             * checks the whole path (mux, enable, output latch) rather than
             * just the write. */
            printf("gpio %d: wrote %d, reads %d\n", pin, val, gpio_read(pin));
            return 0;
        }
    }
}
