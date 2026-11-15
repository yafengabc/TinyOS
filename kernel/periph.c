#include "periph.h"
#include "platform.h"
#include "devfs.h"
#include "console.h"
#include "string.h"

#ifndef TOS_PERIPH
#define TOS_PERIPH 1
#endif

#if TOS_PERIPH

/* Peripheral front end.
 *
 * The kernel keeps no device table: a board implements the handful of
 * plat_gpio_* / plat_adc_* hooks and that is the whole driver model.  What
 * lives here is everything that is *not* board specific:
 *
 *   - the weak defaults, so a board with no GPIO (QEMU virt, the PC, the
 *     mps2) does not have to write five empty functions to say so;
 *   - the range checks, so a user program passing pin 99 gets -1 instead of
 *     writing a random register;
 *   - the two syscalls, which are multiplexed (one number, an op code in a0)
 *     rather than one number per operation: adding a new ADC query later
 *     then costs nothing in the ABI.
 *
 * The hooks are `weak` here and strong in the board file.  lld resolves the
 * strong definition first, so a board that implements GPIO automatically
 * overrides the default without any #ifdef in this file. */

__attribute__((weak)) int plat_gpio_count(void)
{
    return 0;
}

__attribute__((weak)) int plat_gpio_configure(int pin, int mode)
{
    (void)pin; (void)mode;
    return -1;
}

__attribute__((weak)) int plat_gpio_write(int pin, int value)
{
    (void)pin; (void)value;
    return -1;
}

__attribute__((weak)) int plat_gpio_read(int pin)
{
    (void)pin;
    return -1;
}

__attribute__((weak)) int plat_adc_count(void)
{
    return 0;
}

__attribute__((weak)) int plat_adc_read(int chan)
{
    (void)chan;
    return -1;
}

__attribute__((weak)) int plat_adc_vref_mv(void)
{
    return 0;
}

__attribute__((weak)) int plat_adc_bits(void)
{
    return 0;
}

__attribute__((weak)) int plat_pwm_count(void)
{
    return 0;
}

__attribute__((weak)) int plat_pwm_set(int gpio, int freq_hz, int duty_permille)
{
    (void)gpio; (void)freq_hz; (void)duty_permille;
    return -1;
}

__attribute__((weak)) int plat_pwm_off(int gpio)
{
    (void)gpio;
    return -1;
}

__attribute__((weak)) int plat_uart_count(void)
{
    return 0;
}

__attribute__((weak)) int plat_uart_write(int port, const void *buf, int len)
{
    (void)port; (void)buf; (void)len;
    return -1;
}

__attribute__((weak)) int plat_uart_read(int port, void *buf, int len)
{
    (void)port; (void)buf; (void)len;
    return -1;
}

__attribute__((weak)) int plat_spi_count(void)
{
    return 0;
}

__attribute__((weak)) int plat_spi_xfer(int bus, int cs_gpio, int freq_hz,
                                        int mode, const void *tx, void *rx,
                                        int len)
{
    (void)bus; (void)cs_gpio; (void)freq_hz; (void)mode;
    (void)tx; (void)rx; (void)len;
    return -1;
}

__attribute__((weak)) int plat_i2c_count(void)
{
    return 0;
}

__attribute__((weak)) int plat_i2c_xfer(int bus, int addr, int freq_hz,
                                        const void *tx, int txlen,
                                        void *rx, int rxlen)
{
    (void)bus; (void)addr; (void)freq_hz;
    (void)tx; (void)txlen; (void)rx; (void)rxlen;
    return -1;
}

__attribute__((weak)) int plat_pio(int op, struct pio_req *req, int pid)
{
    (void)op; (void)req; (void)pid;
    return -1;
}

__attribute__((weak)) void plat_pio_release(int pid)
{
    (void)pid;
}

__attribute__((weak)) int plat_dma_memcpy(void *dst, const void *src, int len)
{
    (void)dst; (void)src; (void)len;
    return -1;
}

/* ---- syscall surface ------------------------------------------------- */

int periph_gpio(int op, int pin, int arg)
{
    int n = plat_gpio_count();

    if (op == GPIO_OP_COUNT)
        return n;
    if (n <= 0)
        return -1;

    switch (op) {
    case GPIO_OP_MODE:
        if (pin < 0 || pin >= n)
            return -1;
        return plat_gpio_configure(pin, arg);
    case GPIO_OP_WRITE:
        if (pin < 0 || pin >= n)
            return -1;
        return plat_gpio_write(pin, arg != 0);
    case GPIO_OP_READ:
        if (pin < 0 || pin >= n)
            return -1;
        return plat_gpio_read(pin);
    default:
        return -1;
    }
}

int periph_adc(int op, int chan)
{
    int n = plat_adc_count();

    if (op == ADC_OP_COUNT)
        return n;
    if (op == ADC_OP_VREF)
        return plat_adc_vref_mv();
    if (op == ADC_OP_BITS)
        return plat_adc_bits();
    if (op != ADC_OP_READ)
        return -1;
    if (n <= 0 || chan < 0 || chan >= n)
        return -1;
    return plat_adc_read(chan);
}

int periph_pwm(int op, int gpio, int freq_hz, int duty_permille)
{
    int n = plat_gpio_count();

    if (op == PWM_OP_COUNT)
        return plat_pwm_count();
    if (n <= 0 || gpio < 0 || gpio >= n)
        return -1;
    if (op == PWM_OP_OFF)
        return plat_pwm_off(gpio);
    if (op != PWM_OP_SET)
        return -1;
    if (freq_hz <= 0 || duty_permille < 0 || duty_permille > 1000)
        return -1;
    return plat_pwm_set(gpio, freq_hz, duty_permille);
}

int periph_uart_count(void)
{
    return plat_uart_count();
}

int periph_uart_write(int port, const void *buf, int len)
{
    if (port < 0 || port >= plat_uart_count())
        return -1;
    return plat_uart_write(port, buf, len);
}

int periph_uart_read(int port, void *buf, int len)
{
    if (port < 0 || port >= plat_uart_count())
        return -1;
    return plat_uart_read(port, buf, len);
}

int periph_spi_count(void)
{
    return plat_spi_count();
}

int periph_spi_xfer(int bus, int cs_gpio, int freq_hz, int mode,
                    const void *tx, void *rx, int len)
{
    if (bus < 0 || bus >= plat_spi_count())
        return -1;
    if (freq_hz <= 0 || mode < 0 || mode > 7 || len < 0)
        return -1;
    if (cs_gpio < -1 || cs_gpio >= plat_gpio_count())
        return -1;
    if ((!tx && !rx) && len > 0)
        return -1;
    return plat_spi_xfer(bus, cs_gpio, freq_hz, mode, tx, rx, len);
}

int periph_i2c_count(void)
{
    return plat_i2c_count();
}

int periph_i2c_xfer(int bus, int addr, int freq_hz,
                    const void *tx, int txlen, void *rx, int rxlen)
{
    if (bus < 0 || bus >= plat_i2c_count())
        return -1;
    if (addr < 0 || addr > 0x7f || freq_hz <= 0 ||
        txlen < 0 || rxlen < 0)
        return -1;
    if ((!tx && txlen > 0) || (!rx && rxlen > 0))
        return -1;
    return plat_i2c_xfer(bus, addr, freq_hz, tx, txlen, rx, rxlen);
}

int periph_dma_memcpy(void *dst, const void *src, int len)
{
    if (len < 0 || (!dst && len > 0) || (!src && len > 0))
        return -1;
    if (len == 0)
        return 0;
    return plat_dma_memcpy(dst, src, len);
}

/* ---- device files: /dev/gpioN and /dev/adcN -------------------------- */

/* One dev_t per pin/channel, because struct dev carries a void* and handing it
 * a cast integer would be a pointer that is not an object.
 *
 * These used to be 30-odd rows in the device table, one per pin, which is why
 * the table needed 64 slots and a 48-pin part would have overflowed it.  Now
 * each family registers one template ("dev/gpio") and this array supplies the
 * per-instance dev_t the resolver hands back.  The table no longer grows with
 * the pin count; this array does, but it is 16 bytes a pin and lives next to
 * the driver that needs it.
 *
 * The RP2040 has 30 pins and 5 channels; 48 leaves room for the RP2350 without
 * a realloc path that cannot exist (registration happens at boot only). */
#define MAX_PIN_DEVS 48

struct pin_node {
    dev_t dev;
    int   index;                /* pin or channel number, the driver's priv */
};

static struct pin_node gpio_node[MAX_PIN_DEVS];
static struct pin_node adc_node[MAX_PIN_DEVS];

static const dev_t *gpio_resolve(int i) { return &gpio_node[i].dev; }
static const dev_t *adc_resolve(int i)  { return &adc_node[i].dev; }

/* The text form is a digit and a newline - short enough that `echo 1 >
 * /dev/gpio4` (which writes "1\n") and a program writing a bare "1" both
 * work.  Leading whitespace is skipped so a stray space does not become a
 * zero. */
static int parse_level(const char *s, uint32_t len, int *out)
{
    uint32_t i = 0;

    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                       s[i] == '\r'))
        i++;
    if (i >= len)
        return -1;
    if (s[i] == '1') {
        *out = 1;
        return 0;
    }
    if (s[i] == '0') {
        *out = 0;
        return 0;
    }
    return -1;
}

static int gpio_dev_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    char tmp[8];
    int n, v;

    /* A pin is a snapshot, not a stream: after the first read the descriptor
     * is at EOF, so `cat /dev/gpio4` prints one line and exits instead of
     * looping on the same value.  Polling is what the C API is for. */
    if (off)
        return 0;

    v = plat_gpio_read(*(int *)priv);
    if (v < 0)
        return -1;
    n = ksnprintf(tmp, sizeof(tmp), "%d\n", v);
    if (n < 0)
        return -1;
    if ((uint32_t)n > len)
        n = (int)len;
    memcpy(buf, tmp, (size_t)n);
    return n;
}

static int gpio_dev_write(void *priv, uint32_t off, const void *buf,
                          uint32_t len)
{
    int pin = *(int *)priv;
    int level;

    (void)off;
    if (parse_level((const char *)buf, len, &level) < 0)
        return -1;

    /* Writing a pin implies driving it.  The alternative - refusing until an
     * ioctl has set the direction - makes the one-liner people actually
     * type (`echo 1 > /dev/gpio4`) fail, and the pin is theirs either way:
     * if it is already an output this is a no-op, and if it was an input the
     * write could not have meant anything else. */
    if (plat_gpio_configure(pin, PLAT_GPIO_OUT) < 0)
        return -1;
    if (plat_gpio_write(pin, level) < 0)
        return -1;
    return (int)len;              /* all of "1\n" is consumed */
}

static int gpio_dev_ioctl(void *priv, int cmd, int arg)
{
    int pin = *(int *)priv;

    switch (cmd) {
    case GPIO_OP_MODE:
        return plat_gpio_configure(pin, arg);
    case GPIO_OP_WRITE:
        return plat_gpio_write(pin, arg != 0);
    case GPIO_OP_READ:
        return plat_gpio_read(pin);   /* 0/1, no text */
    default:
        return -1;
    }
}

static const struct dev_ops gpio_ops = {
    .read  = gpio_dev_read,
    .write = gpio_dev_write,
    .ioctl = gpio_dev_ioctl,
};

static int adc_dev_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    char tmp[12];
    int n, v;

    if (off)
        return 0;
    v = plat_adc_read(*(int *)priv);
    if (v < 0)
        return -1;
    n = ksnprintf(tmp, sizeof(tmp), "%d\n", v);
    if (n < 0)
        return -1;
    if ((uint32_t)n > len)
        n = (int)len;
    memcpy(buf, tmp, (size_t)n);
    return n;
}

static int adc_dev_write(void *priv, uint32_t off, const void *buf,
                         uint32_t len)
{
    /* An ADC has nothing to write.  Saying so is better than pretending the
     * write landed, because `echo 5 > /dev/adc0` is a mistake, not a mode. */
    (void)priv; (void)off; (void)buf; (void)len;
    return -1;
}

static int adc_dev_ioctl(void *priv, int cmd, int arg)
{
    (void)arg;
    switch (cmd) {
    case ADC_OP_READ: return plat_adc_read(*(int *)priv);
    case ADC_OP_VREF: return plat_adc_vref_mv();
    case ADC_OP_BITS: return plat_adc_bits();
    default:          return -1;
    }
}

static const struct dev_ops adc_ops = {
    .read  = adc_dev_read,
    .write = adc_dev_write,
    .ioctl = adc_dev_ioctl,
};

void periph_init_devs(void)
{
    int ng = plat_gpio_count();
    int na = plat_adc_count();

    if (ng > MAX_PIN_DEVS)
        ng = MAX_PIN_DEVS;
    if (na > MAX_PIN_DEVS)
        na = MAX_PIN_DEVS;

    for (int i = 0; i < ng; i++) {
        gpio_node[i].index    = i;
        gpio_node[i].dev.name = "dev/gpio";   /* the instance is `index` */
        gpio_node[i].dev.ops  = &gpio_ops;
        gpio_node[i].dev.priv = &gpio_node[i].index;
    }
    if (ng > 0)
        devfs_register_template("dev/gpio", ng, gpio_resolve);

    for (int i = 0; i < na; i++) {
        adc_node[i].index    = i;
        adc_node[i].dev.name = "dev/adc";
        adc_node[i].dev.ops  = &adc_ops;
        adc_node[i].dev.priv = &adc_node[i].index;
    }
    if (na > 0)
        devfs_register_template("dev/adc", na, adc_resolve);
}

#else  /* !TOS_PERIPH: no GPIO/ADC/PWM syscalls, no /dev nodes */

int periph_gpio(int op, int pin, int arg)
{
    (void)op; (void)pin; (void)arg;
    return -1;
}

int periph_adc(int op, int chan)
{
    (void)op; (void)chan;
    return -1;
}

int periph_pwm(int op, int gpio, int freq_hz, int duty_permille)
{
    (void)op; (void)gpio; (void)freq_hz; (void)duty_permille;
    return -1;
}

int periph_uart_count(void)
{
    return 0;
}

int periph_uart_write(int port, const void *buf, int len)
{
    (void)port; (void)buf; (void)len;
    return -1;
}

int periph_uart_read(int port, void *buf, int len)
{
    (void)port; (void)buf; (void)len;
    return -1;
}

int periph_spi_count(void)
{
    return 0;
}

int periph_spi_xfer(int bus, int cs_gpio, int freq_hz, int mode,
                    const void *tx, void *rx, int len)
{
    (void)bus; (void)cs_gpio; (void)freq_hz; (void)mode;
    (void)tx; (void)rx; (void)len;
    return -1;
}

int periph_i2c_count(void)
{
    return 0;
}

int periph_i2c_xfer(int bus, int addr, int freq_hz,
                    const void *tx, int txlen, void *rx, int rxlen)
{
    (void)bus; (void)addr; (void)freq_hz;
    (void)tx; (void)txlen; (void)rx; (void)rxlen;
    return -1;
}

int periph_dma_memcpy(void *dst, const void *src, int len)
{
    (void)dst; (void)src; (void)len;
    return -1;
}

void periph_init_devs(void) {}

#endif
