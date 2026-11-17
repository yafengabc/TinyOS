#include "platform.h"
#include "board.h"
#include "board_internal.h"

#ifdef TOS_SPI

/* SPI master on the ESP32-C3's general-purpose SPI2 controller.
 *
 * The C3 has three SPI blocks: SPI0/SPI1 are wired to the flash, and SPI2 is
 * the one on the header.  This driver uses the CPU-controlled ("buffer")
 * mode: the 16-word buffer covers 64 bytes per transaction, so longer
 * transfers are split into 64-byte chunks with the chip select held low
 * across them.
 *
 * Pins (all routed through the GPIO matrix, so they are board policy):
 *
 *   SCK  = GPIO4   -> FSPICLK_OUT (63)
 *   MOSI = GPIO5   -> FSPID_OUT   (65)
 *   MISO = GPIO2   <- FSPIQ_IN    (64, input selector on signal 64)
 *   CS   = a plain GPIO from the caller (hardware CS0 is disabled, same
 *          convention as the RP2040 port, because one bus usually has
 *          several devices)
 *
 * Clock: the module runs from PLL_F80M.  CLKCNT_N divides it,
 * freq = 80 MHz / (pre+1) / (N+1); the pre-divider stays at 0.
 *
 * Data in the buffer is big-endian per 32-bit word (byte 0 in [31:24]),
 * which is also where the first MISO byte lands.
 */

#define SPI_BASE            0x60024000u

#define SPI_CMD             (SPI_BASE + 0x00u)
#define SPI_CTRL            (SPI_BASE + 0x08u)
#define SPI_CLOCK           (SPI_BASE + 0x0cu)
#define SPI_USER            (SPI_BASE + 0x10u)
#define SPI_USER1           (SPI_BASE + 0x14u)
#define SPI_USER2           (SPI_BASE + 0x18u)
#define SPI_MS_DLEN         (SPI_BASE + 0x1cu)
#define SPI_MISC            (SPI_BASE + 0x20u)
#define SPI_DIN_MODE        (SPI_BASE + 0x24u)
#define SPI_DIN_NUM         (SPI_BASE + 0x28u)
#define SPI_W(n)            (SPI_BASE + 0x98u + 4u * (uint32_t)(n))
#define SPI_SLAVE           (SPI_BASE + 0xe0u)
#define SPI_CLK_GATE        (SPI_BASE + 0xe8u)

#define SPI_USR             (1u << 24)
#define SPI_UPDATE          (1u << 23)

#define SPI_WR_BIT_ORDER    (1u << 26)
#define SPI_RD_BIT_ORDER    (1u << 25)

#define SPI_CLK_EQU_SYSCLK  (1u << 31)
#define SPI_CLKDIV_PRE_S    18
#define SPI_CLKCNT_N_S      12
#define SPI_CLKCNT_H_S      6
#define SPI_CLKCNT_L_S      0

#define SPI_USR_COMMAND     (1u << 31)
#define SPI_USR_ADDR        (1u << 30)
#define SPI_USR_DUMMY       (1u << 29)
#define SPI_USR_MISO        (1u << 28)
#define SPI_USR_MOSI        (1u << 27)
#define SPI_CK_OUT_EDGE     (1u << 9)
#define SPI_DOUTDIN         (1u << 0)

#define SPI_CS_DIS_ALL      0x3fu          /* CS0..CS5 disabled            */
#define SPI_CK_IDLE_EDGE    (1u << 29)
#define SPI_SLAVE_MODE      (1u << 26)

#define SYSTEM_BASE         0x60026000u
#define PERIP_CLK_EN0       (SYSTEM_BASE + 0x10u)
#define PERIP_RST_EN0       (SYSTEM_BASE + 0x18u)
#define PERIP_SPI2          (1u << 6)
#define PERIP_SPI2_DMA      (1u << 22)

#define GPIO_BASE           0x60004000u
#define GPIO_FUNC_OUT(n)    (GPIO_BASE + 0x554u + 4u * (uint32_t)(n))
#define GPIO_FUNC_IN(sig)   (GPIO_BASE + 0x154u + 4u * (uint32_t)(sig))
#define GPIO_IN_SEL_EN      (1u << 6)

#define SIG_FSPICLK_OUT     63u
#define SIG_FSPIQ_IN        64u
#define SIG_FSPID_OUT       65u

#define SPI_SCK_PIN         4
#define SPI_MOSI_PIN        5
#define SPI_MISO_PIN        2

#define SPI_CHUNK           64             /* 16 words */

static int g_ready;

static void spi_init_once(void)
{
    uint32_t clk_gate;

    if (g_ready)
        return;

    mmio_write32(PERIP_CLK_EN0,
                 mmio_read32(PERIP_CLK_EN0) | PERIP_SPI2 | PERIP_SPI2_DMA);
    mmio_write32(PERIP_RST_EN0, mmio_read32(PERIP_RST_EN0) & ~PERIP_SPI2);

    /* CLK_EN gates the register file, MST_CLK_ACTIVE powers the module.
     * MST_CLK_SEL: 1 = PLL_F80M, 0 = XTAL.  Use the PLL when APB came up at
     * 80 MHz, which is the normal case on this port. */
    clk_gate = (1u << 0) | (1u << 1);
    if (c3_apb_hz() >= 80000000u)
        clk_gate |= (1u << 2);
    mmio_write32(SPI_CLK_GATE, clk_gate);

    mmio_write32(SPI_SLAVE, mmio_read32(SPI_SLAVE) & ~SPI_SLAVE_MODE);
    mmio_write32(SPI_CTRL, 0);                  /* MSB first, single line  */
    mmio_write32(SPI_MISC, SPI_CS_DIS_ALL);     /* CS is a plain GPIO      */
    mmio_write32(SPI_DIN_MODE, 0);
    mmio_write32(SPI_DIN_NUM, 0);
    mmio_write32(SPI_USER1, 0);
    mmio_write32(SPI_USER2, 0);

    g_ready = 1;
}

static void spi_pins_once(void)
{
    /* Output enable for SCK/MOSI comes from the SPI peripheral, so write the
     * matrix selector without the OEN_SEL bit that plat_gpio_configure sets
     * for a plain GPIO. */
    plat_gpio_configure(SPI_SCK_PIN, PLAT_GPIO_OUT);
    mmio_write32(GPIO_FUNC_OUT(SPI_SCK_PIN), SIG_FSPICLK_OUT);

    plat_gpio_configure(SPI_MOSI_PIN, PLAT_GPIO_OUT);
    mmio_write32(GPIO_FUNC_OUT(SPI_MOSI_PIN), SIG_FSPID_OUT);

    plat_gpio_configure(SPI_MISO_PIN, PLAT_GPIO_IN_PULLUP);
    mmio_write32(GPIO_FUNC_IN(SIG_FSPIQ_IN),
                 (uint32_t)SPI_MISO_PIN | GPIO_IN_SEL_EN);
}

static int spi_apply(int freq_hz, int mode)
{
    uint32_t base = (c3_apb_hz() >= 80000000u) ? 80000000u : 40000000u;
    uint32_t n, user, misc;

    if (freq_hz <= 0 || mode < 0 || mode > 3)
        return -1;
    if ((uint32_t)freq_hz > base / 2u)
        freq_hz = (int)(base / 2u);

    n = base / (uint32_t)freq_hz;               /* (N+1) */
    if (n < 2u)
        n = 2u;
    if (n > 64u)
        n = 64u;
    n -= 1u;                                    /* N = clkcnt */

    user = SPI_DOUTDIN | SPI_USR_MOSI | SPI_USR_MISO;
    if (mode & 1)
        user |= SPI_CK_OUT_EDGE;                /* CPHA */
    misc = SPI_CS_DIS_ALL;
    if (mode & 2)
        misc |= SPI_CK_IDLE_EDGE;               /* CPOL */

    mmio_write32(SPI_CLOCK,
                 (((n + 1u) / 2u - 1u) << SPI_CLKCNT_H_S) |
                 (n << SPI_CLKCNT_N_S) |
                 (n << SPI_CLKCNT_L_S));        /* EQU_SYSCLK=0, PRE=0 */
    mmio_write32(SPI_USER, user);
    mmio_write32(SPI_MISC, misc);
    mmio_write32(SPI_CMD, SPI_UPDATE);
    for (uint32_t i = 0; i < 10000u; i++)
        if (!(mmio_read32(SPI_CMD) & SPI_UPDATE))
            break;
    return 0;
}

static void spi_put_word(uint32_t idx, const uint8_t *p, int n)
{
    uint32_t w = 0;

    for (int i = 0; i < 4; i++)
        w |= (uint32_t)((p && i < n) ? p[i] : 0u) << (24 - 8 * i);
    mmio_write32(SPI_W(idx), w);
}

static void spi_get_word(uint32_t idx, uint8_t *p, int n)
{
    uint32_t w = mmio_read32(SPI_W(idx));

    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(w >> (24 - 8 * i));
}

int plat_spi_count(void)
{
    return 1;
}

int plat_spi_xfer(int bus, int cs_gpio, int freq_hz, int mode,
                  const void *tx, void *rx, int len)
{
    const uint8_t *out = (const uint8_t *)tx;
    uint8_t *in = (uint8_t *)rx;

    if (bus != 0 || len < 0)
        return -1;
    if (len > 0 && !out && !in)
        return -1;

    spi_init_once();
    spi_pins_once();
    if (spi_apply(freq_hz, mode) < 0)
        return -1;

    if (cs_gpio >= 0) {
        if (plat_gpio_configure(cs_gpio, PLAT_GPIO_OUT) < 0)
            return -1;
        plat_gpio_write(cs_gpio, 1);
        plat_gpio_write(cs_gpio, 0);
    }

    int ok = 1;
    for (int off = 0; off < len; off += SPI_CHUNK) {
        int chunk = len - off;
        if (chunk > SPI_CHUNK)
            chunk = SPI_CHUNK;

        for (int i = 0; i < chunk; i += 4) {
            int n = chunk - i;
            if (n > 4)
                n = 4;
            spi_put_word((uint32_t)(i / 4), out ? out + off + i : 0, n);
        }

        mmio_write32(SPI_MS_DLEN, (uint32_t)chunk * 8u - 1u);
        mmio_write32(SPI_CMD, SPI_UPDATE);
        for (uint32_t i = 0; i < 10000u; i++)
            if (!(mmio_read32(SPI_CMD) & SPI_UPDATE))
                break;
        mmio_write32(SPI_CMD, SPI_USR);

        int done = 0;
        for (uint32_t i = 0; i < 2000000u; i++) {
            if (!(mmio_read32(SPI_CMD) & SPI_USR)) {
                done = 1;
                break;
            }
        }
        if (!done) {
            ok = 0;
            break;
        }

        if (in) {
            for (int i = 0; i < chunk; i += 4) {
                int n = chunk - i;
                if (n > 4)
                    n = 4;
                spi_get_word((uint32_t)(i / 4), in + off + i, n);
            }
        }
    }

    if (cs_gpio >= 0)
        plat_gpio_write(cs_gpio, 1);
    return ok ? len : -1;
}

#endif /* TOS_SPI */
