#include "platform.h"

#ifdef TOS_I2C

/* I2C master on the ESP32-C3, bit-banged over two GPIOs.
 *
 * The C3 has one hardware I2C controller (I2C0 at 0x60013000), but driving
 * it means programming the command FIFO (op_code/ack-en/byte_num words),
 * which is a whole second protocol on top of I2C.  For bring-up, and for the
 * speeds sensors actually use, plain open-drain bit-banging is both shorter
 * and easier to trust: every step is one pad read or write, and a missing
 * pull-up shows up as a NACK instead of a wedged state machine.
 *
 * Pins are the C3 module's documented defaults: SDA = GPIO8, SCL = GPIO9.
 * Both are open-drain: "high" means "release the pad and let the pull-up (or
 * the external resistor) do it", "low" means drive 0.  SCL is released the
 * same way so a slave can stretch the clock; every wait for SCL high is
 * bounded, so a shorted bus returns -1 rather than hanging the kernel.
 *
 * Speed: `freq_hz` is honoured up to 400 kHz by scaling the quarter-bit
 * delay from plat_cpu_hz().  The pads' rise time with the internal ~45 kOhm
 * pull-up limits the useful rate well below that; a device with external
 * 4.7 kOhm resistors is happy at 100-400 kHz.
 */

#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9

#define I2C_STRETCH_LIMIT 200000u

static uint32_t g_quarter;      /* delay-loop iterations per quarter bit */

static void i2c_delay(void)
{
    for (volatile uint32_t i = 0; i < g_quarter; i++)
        ;
}

static void sda_low(void)
{
    plat_gpio_configure(I2C_SDA_PIN, PLAT_GPIO_OUT);
    plat_gpio_write(I2C_SDA_PIN, 0);
}

static void sda_release(void)
{
    plat_gpio_configure(I2C_SDA_PIN, PLAT_GPIO_IN_PULLUP);
}

static void scl_low(void)
{
    plat_gpio_configure(I2C_SCL_PIN, PLAT_GPIO_OUT);
    plat_gpio_write(I2C_SCL_PIN, 0);
}

/* Release SCL and wait for it to actually read high: a slave may hold it
 * down to stretch the clock, and pushing on regardless would corrupt the
 * byte we are shifting. */
static int scl_release(void)
{
    plat_gpio_configure(I2C_SCL_PIN, PLAT_GPIO_IN_PULLUP);
    for (uint32_t i = 0; i < I2C_STRETCH_LIMIT; i++)
        if (plat_gpio_read(I2C_SCL_PIN) == 1)
            return 0;
    return -1;
}

static int i2c_start(void)
{
    sda_release();
    if (scl_release() < 0)
        return -1;
    i2c_delay();
    if (plat_gpio_read(I2C_SDA_PIN) != 1)
        return -1;                  /* bus held low: no pull-up/short */
    sda_low();
    i2c_delay();
    scl_low();
    i2c_delay();
    return 0;
}

static void i2c_stop(void)
{
    sda_low();
    i2c_delay();
    if (scl_release() == 0)
        i2c_delay();
    sda_release();
    i2c_delay();
}

/* Returns 0 when the slave acknowledged, -1 on NACK. */
static int i2c_write_byte(uint8_t b)
{
    for (int bit = 7; bit >= 0; bit--) {
        scl_low();
        if ((b >> bit) & 1u)
            sda_release();
        else
            sda_low();
        i2c_delay();
        if (scl_release() < 0)
            return -1;
        i2c_delay();
    }

    /* Ninth clock: release SDA and sample it. */
    scl_low();
    sda_release();
    i2c_delay();
    if (scl_release() < 0)
        return -1;
    i2c_delay();
    int ack = (plat_gpio_read(I2C_SDA_PIN) == 0);
    scl_low();
    i2c_delay();
    return ack ? 0 : -1;
}

static int i2c_read_byte(int ack)
{
    uint8_t v = 0;

    sda_release();
    for (int bit = 7; bit >= 0; bit--) {
        scl_low();
        i2c_delay();
        if (scl_release() < 0)
            return -1;
        i2c_delay();
        v = (uint8_t)((v << 1) | (plat_gpio_read(I2C_SDA_PIN) & 1u));
    }

    /* ACK (0) or NACK (1) for the byte just read. */
    scl_low();
    if (ack)
        sda_low();
    else
        sda_release();
    i2c_delay();
    if (scl_release() < 0)
        return -1;
    i2c_delay();
    scl_low();
    sda_release();
    i2c_delay();
    return v;
}

int plat_i2c_count(void)
{
    return 1;
}

int plat_i2c_xfer(int bus, int addr, int freq_hz,
                  const void *tx, int txlen, void *rx, int rxlen)
{
    const uint8_t *out = (const uint8_t *)tx;
    uint8_t *in = (uint8_t *)rx;
    uint32_t cpu;

    if (bus != 0 || addr < 0 || addr > 0x7f || txlen < 0 || rxlen < 0)
        return -1;
    if ((!out && txlen > 0) || (!in && rxlen > 0))
        return -1;
    if (txlen == 0 && rxlen == 0)
        return -1;                  /* probe: read one byte instead */

    if (freq_hz <= 0)
        freq_hz = 100000;
    if (freq_hz > 400000)
        freq_hz = 400000;

    /* Each delay-loop iteration is ~3 cycles; a quarter bit is 1/(4*f). */
    cpu = plat_cpu_hz();
    g_quarter = cpu / ((uint32_t)freq_hz * 12u);
    if (g_quarter < 2)
        g_quarter = 2;

    for (int i = 0; i < 3; i++) {           /* bus recovery: 3 idle clocks */
        sda_release();
        scl_low();
        i2c_delay();
        scl_release();
        i2c_delay();
    }

    if (i2c_start() < 0)
        goto fail;

    if (i2c_write_byte((uint8_t)((addr << 1) | (rxlen ? 1 : 0))) < 0)
        goto fail;

    for (int i = 0; i < txlen; i++)
        if (i2c_write_byte(out[i]) < 0)
            goto fail;

    if (rxlen > 0) {
        if (txlen > 0) {
            /* Repeated START: SDA is already released and SCL is low after
             * the last ACK, which is exactly the state i2c_start() expects. */
            if (i2c_start() < 0)
                goto fail;
            if (i2c_write_byte((uint8_t)((addr << 1) | 1)) < 0)
                goto fail;
        }
        for (int i = 0; i < rxlen; i++) {
            int v = i2c_read_byte(i < rxlen - 1);   /* ACK all but the last */
            if (v < 0)
                goto fail;
            in[i] = (uint8_t)v;
        }
    }

    i2c_stop();
    return txlen + rxlen;

fail:
    i2c_stop();
    return -1;
}

#endif /* TOS_I2C */
