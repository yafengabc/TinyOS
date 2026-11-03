#include "platform.h"
#include "board.h"

/* GPIO for the ESP32-C3.
 *
 * The C3 does not have a "SIO"-style pin block: every pad goes through the
 * IO_MUX and then through the GPIO matrix, which can route any peripheral
 * signal to any pad.  For plain software-driven I/O the route is fixed:
 *
 *   1. IO_MUX[n].MCU_SEL = 1     "this pad belongs to the GPIO peripheral"
 *   2. IO_MUX[n].FUN_IE          input enable (readable at all)
 *   3. GPIO_FUNCn_OUT_SEL = 128  the pad's output is driven by GPIO_OUT
 *   4. GPIO_ENABLE_W1TS bit n    output driver on (with OEN from the reg)
 *   5. GPIO_OUT_W1TS/W1TC        set / clear, one bit per pin
 *
 * Everything is write-1-to-set / write-1-to-clear on purpose: it means a
 * pin operation never has to read-modify-write a shared register, which is
 * exactly what makes the same calls safe from two cores later.
 *
 * Register map from the ESP32-C3 TRM (chapter "GPIO Matrix and IO_MUX");
 * see the note in board/esp32c3/platform.c about which parts of this port
 * have been exercised on silicon.  The GPIO matrix select value (128,
 * "IGPIO") is the one part that differs between ESP32 generations - it is
 * 256 on the original ESP32 and 128 on the C3/S3 family. */

#define GPIO_BASE        0x60004000u
#define GPIO_OUT_REG     (GPIO_BASE + 0x04)
#define GPIO_OUT_W1TS    (GPIO_BASE + 0x08)
#define GPIO_OUT_W1TC    (GPIO_BASE + 0x0C)
#define GPIO_ENABLE_W1TS (GPIO_BASE + 0x24)
#define GPIO_ENABLE_W1TC (GPIO_BASE + 0x28)
#define GPIO_IN_REG      (GPIO_BASE + 0x3C)
#define GPIO_FUNC_OUT(n) (GPIO_BASE + 0x554u + 4u * (uint32_t)(n))

#define IO_MUX_BASE      0x60009000u
#define IO_MUX(n)        (IO_MUX_BASE + 4u * (uint32_t)(n))

#define IO_MUX_FUN_WPD   (1u << 7)
#define IO_MUX_FUN_WPU   (1u << 8)
#define IO_MUX_FUN_IE    (1u << 9)
#define IO_MUX_MCU_SEL_S 12
#define IO_MUX_MCU_SEL_M (7u << 12)
#define IO_MUX_MCU_SEL_GPIO (1u << 12)

#define SIG_GPIO_OUT_IDX 128u     /* "output = the GPIO_OUT register" */

/* GPIO0..21 are bonded out on the C3.  The matrix has entries for all of
 * them; pins 22.. are internal. */
#define C3_GPIO_PINS 22

int plat_gpio_count(void)
{
    return C3_GPIO_PINS;
}

int plat_gpio_configure(int pin, int mode)
{
    uint32_t mux;

    if (pin < 0 || pin >= C3_GPIO_PINS)
        return -1;

    mux = mmio_read32(IO_MUX(pin)) & ~(IO_MUX_MCU_SEL_M | IO_MUX_FUN_IE |
                                       IO_MUX_FUN_WPU | IO_MUX_FUN_WPD);
    mux |= IO_MUX_MCU_SEL_GPIO;

    switch (mode) {
    case PLAT_GPIO_IN:
        mux |= IO_MUX_FUN_IE;
        /* Output driver off; the pad is only an input now. */
        mmio_write32(GPIO_ENABLE_W1TC, 1u << pin);
        break;
    case PLAT_GPIO_IN_PULLUP:
        mux |= IO_MUX_FUN_IE | IO_MUX_FUN_WPU;
        mmio_write32(GPIO_ENABLE_W1TC, 1u << pin);
        break;
    case PLAT_GPIO_IN_PULLDOWN:
        mux |= IO_MUX_FUN_IE | IO_MUX_FUN_WPD;
        mmio_write32(GPIO_ENABLE_W1TC, 1u << pin);
        break;
    case PLAT_GPIO_OUT:
        /* Input enable stays ON so the pin can be read back: it costs a few
         * microamps and it makes `gpio 4 1; gpio 4` mean what a user expects
         * (the level the pin is driving), which is also how the emulator
         * models the pad.
         *
         * One register per pad: [7:0] which signal drives the output, [8]
         * whether the output enable comes from the GPIO peripheral (that is
         * what makes GPIO_ENABLE_W1TS matter), [9]/[10] the inversions. */
        mux |= IO_MUX_FUN_IE;
        mmio_write32(GPIO_FUNC_OUT(pin), SIG_GPIO_OUT_IDX | (1u << 8));
        mmio_write32(GPIO_ENABLE_W1TS, 1u << pin);
        break;
    default:
        return -1;
    }

    mmio_write32(IO_MUX(pin), mux);
    return 0;
}

int plat_gpio_write(int pin, int value)
{
    if (pin < 0 || pin >= C3_GPIO_PINS)
        return -1;
    mmio_write32(value ? GPIO_OUT_W1TS : GPIO_OUT_W1TC, 1u << pin);
    return 0;
}

int plat_gpio_read(int pin)
{
    if (pin < 0 || pin >= C3_GPIO_PINS)
        return -1;
    return (mmio_read32(GPIO_IN_REG) >> pin) & 1u;
}
