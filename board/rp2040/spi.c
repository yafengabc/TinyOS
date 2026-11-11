#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "cpu.h"

#ifdef TOS_SPI

/* SPI0 and SPI1, master mode, 8-bit, full duplex.
 *
 * The PL022 register block is the same on both instances.  SCK/MOSI/MISO
 * are owned by the controller (FUNCSEL 1); the chip select is a normal GPIO
 * driven by software, because the Pico does not wire the PL022's hardware
 * CS and a shared bus needs per-device selects anyway.
 *
 * clk_peri is 125 MHz.  The PL022 has an even prescaler 2..254 and an 8-bit
 * post-divider, so the slowest clock is about 1.9 kHz; asking for less is
 * rejected rather than silently running faster. */

#define SPI_CR0   0x00u
#define SPI_CR1   0x04u
#define SPI_DR    0x08u
#define SPI_SR    0x0cu
#define SPI_CPSR  0x10u
#define SPI_ICR   0x20u

#define SR_TNF  (1u << 1)
#define SR_RNE  (1u << 2)
#define SR_RFF  (1u << 3)
#define SR_BSY  (1u << 4)

#define CR0_DSS8  7u
#define CR0_SPO   (1u << 6)
#define CR0_SPH   (1u << 7)
#define CR1_LBM   (1u << 0)      /* internal MOSI->MISO loopback */
#define CR1_SSE   (1u << 1)

#define IO_BANK0_GPIO_CTRL(n) (IO_BANK0_BASE + 0x04u + 8u * (uint32_t)(n))
#define PADS_BANK0_GPIO(n)    (PADS_BANK0_BASE + 0x04u + 4u * (uint32_t)(n))

#define PAD_OD  (1u << 7)
#define PAD_IE  (1u << 6)
#define PAD_PUE (1u << 3)
#define PAD_PDE (1u << 2)
#define PAD_ISO (1u << 8)
#define FUNCSEL_SPI 1u

static const uint32_t spi_base[2] = { SPI0_BASE, SPI1_BASE };
static const uint32_t spi_reset[2] = { RESET_SPI0, RESET_SPI1 };
static const int spi_pins[2][3] = {
    { 2, 3, 4 },       /* SPI0: SCK, TX, RX */
    { 10, 11, 12 },    /* SPI1: SCK, TX, RX */
};

static spin_t spi_lock[2] = { SPIN_INIT, SPIN_INIT };
static int spi_ready[2];

static void spi_pin_init(int pin)
{
    uint32_t pad = mmio_read32(PADS_BANK0_GPIO(pin));

    pad &= ~(PAD_OD | PAD_PUE | PAD_PDE | PAD_ISO);
    pad |= PAD_IE;
    mmio_write32(PADS_BANK0_GPIO(pin), pad);
    mmio_write32(IO_BANK0_GPIO_CTRL(pin), FUNCSEL_SPI);
}

static void spi_init_once(int bus)
{
    uint32_t v, spins, base = spi_base[bus];

    if (spi_ready[bus])
        return;

    v = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    if (v & spi_reset[bus]) {
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                     v & ~spi_reset[bus]);
        for (spins = 0; spins < 1000000u; spins++)
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
                spi_reset[bus])
                break;
    }

    for (int i = 0; i < 3; i++)
        spi_pin_init(spi_pins[bus][i]);

    mmio_write32(base + SPI_CR1, 0);
    mmio_write32(base + SPI_CR0, CR0_DSS8);
    mmio_write32(base + SPI_CPSR, 2);
    mmio_write32(base + SPI_CR1, CR1_SSE);
    spi_ready[bus] = 1;
}

static int spi_apply(int bus, int freq_hz, int mode)
{
    uint32_t base = spi_base[bus];
    uint32_t prescale, postdiv, denom, cr0;

    if (freq_hz <= 0 || mode < 0 || mode > 7)
        return -1;

    /* Smallest even prescaler that makes the post-divider fit in 8 bits. */
    for (prescale = 2; prescale <= 254; prescale += 2) {
        if ((uint32_t)freq_hz >=
            RP2040_SYS_CLK_HZ / (prescale * 256u))
            break;
    }
    if (prescale > 254)
        return -1;

    denom = prescale * (uint32_t)freq_hz;
    postdiv = RP2040_SYS_CLK_HZ / denom;
    if (postdiv < 1u)
        postdiv = 1u;
    if (postdiv > 256u)
        postdiv = 256u;

    cr0 = CR0_DSS8 | ((postdiv - 1u) << 8);
    if (mode & 1)
        cr0 |= CR0_SPO;
    if (mode & 2)
        cr0 |= CR0_SPH;

    mmio_write32(base + SPI_CR1, 0);
    mmio_write32(base + SPI_CR0, cr0);
    mmio_write32(base + SPI_CPSR, prescale);
    mmio_write32(base + SPI_ICR, 1);
    mmio_write32(base + SPI_CR1,
                 CR1_SSE | ((mode & 4) ? CR1_LBM : 0u));
    return 0;
}

int plat_spi_count(void)
{
    return 2;
}

int plat_spi_xfer(int bus, int cs_gpio, int freq_hz, int mode,
                  const void *tx, void *rx, int len)
{
    const uint8_t *src = (const uint8_t *)tx;
    uint8_t *dst = (uint8_t *)rx;
    uint32_t base;
    int cs_ok = 0;
    uint32_t spins;

    if (bus < 0 || bus >= 2 || len < 0)
        return -1;

    if (cs_gpio >= 0) {
        for (int i = 0; i < 3; i++)
            if (cs_gpio == spi_pins[bus][i])
                return -1;      /* a CS cannot also be SCK/MOSI/MISO */
    }

    spin_lock(&spi_lock[bus]);
    spi_init_once(bus);
    if (spi_apply(bus, freq_hz, mode) < 0) {
        spin_unlock(&spi_lock[bus]);
        return -1;
    }

    base = spi_base[bus];
    if (cs_gpio >= 0) {
        if (plat_gpio_configure(cs_gpio, PLAT_GPIO_OUT) < 0) {
            spin_unlock(&spi_lock[bus]);
            return -1;
        }
        plat_gpio_write(cs_gpio, 1);
        plat_gpio_write(cs_gpio, 0);
        cs_ok = 1;
    }

    for (int i = 0; i < len; i++) {
        uint8_t out = src ? src[i] : 0xFFu;

        for (spins = 0; spins < 1000000u; spins++)
            if (mmio_read32(base + SPI_SR) & SR_TNF)
                break;
        if (spins == 1000000u)
            goto fail;

        mmio_write32(base + SPI_DR, out);

        for (spins = 0; spins < 1000000u; spins++)
            if (mmio_read32(base + SPI_SR) & SR_RNE)
                break;
        if (spins == 1000000u)
            goto fail;

        if (dst)
            dst[i] = (uint8_t)(mmio_read32(base + SPI_DR) & 0xFFu);
        else
            (void)mmio_read32(base + SPI_DR);
    }

    for (spins = 0; spins < 1000000u; spins++)
        if (!(mmio_read32(base + SPI_SR) & SR_BSY))
            break;

    if (cs_ok)
        plat_gpio_write(cs_gpio, 1);
    spin_unlock(&spi_lock[bus]);
    return len;

fail:
    if (cs_ok)
        plat_gpio_write(cs_gpio, 1);
    spin_unlock(&spi_lock[bus]);
    return -1;
}

#endif /* TOS_SPI */
