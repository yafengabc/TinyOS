#include "platform.h"
#include "rp2350rv_memmap.h"
#include "board.h"
#include "spi.h"

/* Polled SPI master for the RP2350's two PL022 blocks.
 *
 * Polled, not interrupt driven, for the same reason the USB CDC console
 * is: short command bytes cost more in overhead than they would gain
 * from an interrupt.  Bulk pixel traffic does NOT go through here any
 * more - the panel drives DMA channel 10 straight into the TX FIFO and
 * only lazily waits for the tail (lcd_st7789.c).
 *
 * clk_peri follows clk_sys, which plat_board_early() hosts on PLL_SYS at
 * 150 MHz (falling back to the ROM's ~12 MHz if the PLL refuses to lock).
 * Callers pick their own CPSDVSR: the panel uses 4 for 37.5 MHz, well
 * inside the ST7789's write timing, so a full text row is ~0.8 ms of
 * wire time. */

static uint32_t spi_base(int bus)
{
    return bus == 1 ? SPI1_BASE : SPI0_BASE;
}

void rp_spi_init(int bus, uint32_t div)
{
    uint32_t base = spi_base(bus);
    uint32_t bit = bus == 1 ? RESET_SPI1 : RESET_SPI0;

    /* Out of reset.  Like every other peripheral on this chip, the block
     * stays in reset until the bit is cleared, and touching its registers
     * while it is held stalls the bus. */
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~bit);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & bit)
            break;
    }

    mmio_write32(base + RP_SPI_CR1, 0u);        /* disable while setting up */
    mmio_write32(base + RP_SPI_CPSR, div < 2u ? 2u : div);
    /* 8-bit frames, SPI mode 0, Motorola frame format, SCR = 0. */
    mmio_write32(base + RP_SPI_CR0, RP_SPI_CR0_DSS_8);
    mmio_write32(base + RP_SPI_CR1, RP_SPI_CR1_SSE);
    /* Let the block assert its TX/RX FIFO DMA requests.  Without TXDMAE
     * the DREQ line stays low and a DMA channel paced on it never
     * completes - pico-sdk does this in spi_init() too.  Polled writes
     * are unaffected. */
    mmio_write32(base + RP_SPI_DMACR,
                 RP_SPI_DMACR_TXDMAE | RP_SPI_DMACR_RXDMAE);
}

int rp_spi_write(int bus, const void *buf, size_t n)
{
    uint32_t base = spi_base(bus);
    const uint8_t *p = (const uint8_t *)buf;

    for (size_t i = 0; i < n; i++) {
        uint32_t spins = 200000u;

        while (!(mmio_read32(base + RP_SPI_SR) & RP_SPI_SR_TNF)) {
            if (--spins == 0)
                return -1;
        }
        mmio_write32(base + RP_SPI_DR, p[i]);
        /* The receive side shifts in whatever the panel drives; nothing
         * reads it, and a full 8-entry RX FIFO would stall the next write,
         * so drop it as we go. */
        if (mmio_read32(base + RP_SPI_SR) & RP_SPI_SR_RNE)
            (void)mmio_read32(base + RP_SPI_DR);
    }

    /* Idle before the caller drops chip select: releasing CS while the
     * shift register is still running truncates the last byte. */
    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        if (!(mmio_read32(base + RP_SPI_SR) & RP_SPI_SR_BSY))
            break;
    }
    while (mmio_read32(base + RP_SPI_SR) & RP_SPI_SR_RNE)
        (void)mmio_read32(base + RP_SPI_DR);
    return (int)n;
}

int rp_spi_write_byte(int bus, uint8_t b)
{
    return rp_spi_write(bus, &b, 1);
}
