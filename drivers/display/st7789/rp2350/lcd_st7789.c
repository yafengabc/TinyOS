#include "platform.h"
#include "hwdev.h"
#include "arm_memmap.h"
#include "board.h"
#include "spi.h"
#include "font5x7.h"

/* ST7789V2 panel on the ATK-DNRP2350AM.
 *
 * The board carries a 1.14" 135x240 IPS panel wired as 4-wire SPI:
 *
 *   SCK  GPIO10   (SPI1 SCLK)
 *   MOSI GPIO11   (SPI1 TX)
 *   DC   GPIO8    (0 = command, 1 = data)
 *   CS   GPIO9
 *   BL   GPIO25   (backlight, active LOW - see below)
 *   RST  tied to the board's RESET line, so there is no software reset pin
 *
 * The backlight is the one polarity worth reading off the schematic twice:
 * Q2 is an S8550 PNP whose emitter is on 3V3, whose base comes from GPIO25
 * through 1K with a 10K pull-up, and whose collector feeds LEDA through
 * 10R.  Pulling the base low turns the transistor on, so GPIO25 = 0 is
 * "lit" and 1 is "dark" - the opposite of what the pin name suggests.
 *
 * Note the pin sharing: GPIO10/11 are also SDIO CLK/CMD for the TF card
 * slot.  The two cannot be used at once, which is why the board's device
 * table marks the card slot TOS_DEV_SHARED.
 *
 * This file is the pixel layer only: it can paint one character cell, a
 * whole text row and fill the screen.  Cursor, scrolling and the console
 * mirror live in kernel/screen.c, so a different panel only has to
 * implement the hooks in hwdev.h.
 *
 * Big transfers (a text row, a clear) go out through DMA channel 10 and
 * the CPU keeps running: a caller queues pixels and returns, and the next
 * transaction lazily waits for the tail.  plat_display_sync() is the gate
 * anything sharing GPIO10/11 with this panel must call before touching
 * the chip-selects.
 *
 * MADCTL 0x70 is the landscape orientation the vendor code uses; the
 * controller's RAM is 240x320 behind a 135x240 glass, so every window is
 * offset by (40, 53) - the usual calibration for this panel family. */

#define LCD_SPI_BUS   1
#define LCD_SPI_DIV   4         /* 150 MHz / 4 = 37.5 MHz.  The ST7789 takes
                                 * writes up to ~62 MHz and the traces here
                                 * are a centimetre long; 37.5 keeps a safe
                                 * margin.  A full text row (3840 bytes) is
                                 * then ~0.8 ms of wire time, and scrolling
                                 * the 16-row screen costs ~13 ms of bus,
                                 * none of it polled by the CPU (DMA does
                                 * the walking). */

#define LCD_PIN_SCK   10
#define LCD_PIN_MOSI  11
#define LCD_PIN_DC    8
#define LCD_PIN_CS    9
#define LCD_PIN_BL    25

#define LCD_W         240
#define LCD_H         135
#define LCD_CELL_W    6
#define LCD_CELL_H    8

#define LCD_XOFF      40
#define LCD_YOFF      53

#define LCD_COLS      (LCD_W / LCD_CELL_W)
#define LCD_ROWS      (LCD_H / LCD_CELL_H)

static int lcd_up;              /* init ran and succeeded                 */
static int bl_state = -1;
static int pins_ready;          /* CS/DC pads configured, SIO owns them  */

/* ---- pin plumbing ----------------------------------------------------- */

static void pin_func(int pin, uint32_t fn)
{
    uint32_t pad = mmio_read32(RP_GPIO_PAD(pin));

    mmio_write32(RP_GPIO_CTRL(pin), fn);
    /* ISO resets to 1 on every RP2350 pad: clearing it is what actually
     * connects the pin (see arm_memmap.h). */
    mmio_write32(RP_GPIO_PAD(pin),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
}

static void pin_out(int pin, int level)
{
    uint32_t pad = mmio_read32(RP_GPIO_PAD(pin));

    mmio_write32(RP_GPIO_CTRL(pin), RP_GPIO_FUNC_SIO);
    mmio_write32(RP_GPIO_PAD(pin),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
    mmio_write32(level ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR, 1u << pin);
    mmio_write32(SIO_GPIO_OE_SET, 1u << pin);
}

/* CS/DC are toggled several times per character.  Re-running pin_out()
 * each time would re-program FUNCSEL and the PAD register - and the pad
 * control registers live on a slow, synchronised domain, so ~90 such
 * accesses cost more than the pixels themselves.  Once the pads are set
 * up, a level change is one SIO write. */
static void lcd_cs(int level)
{
    if (!pins_ready) {
        pin_out(LCD_PIN_CS, level);
        return;
    }
    mmio_write32(level ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR,
                 1u << LCD_PIN_CS);
}

static void lcd_dc(int level)
{
    if (!pins_ready) {
        pin_out(LCD_PIN_DC, level);
        return;
    }
    mmio_write32(level ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR,
                 1u << LCD_PIN_DC);
}

/* ---- byte channel ----------------------------------------------------- */

/* DMA channel 10 walks a RAM buffer into SPI1's TX FIFO; channel 11 is
 * left for the TF card.  Armed transfers are finished lazily: the next
 * transaction on this bus waits for the tail before touching CS, so a
 * caller that queued pixels pays nothing and a caller that wants the bus
 * back pays only the outstanding wire time. */
#define LCD_DMA_CH    10

static int dma_up;              /* reset released, channel configured    */
static int dma_armed;           /* pixels still in flight                 */
static uint8_t rowbuf[LCD_COLS * LCD_CELL_W * LCD_CELL_H * 2];  /* 3840 B */
static uint8_t cellbuf[LCD_CELL_W * LCD_CELL_H * 2];           /* 96 B   */

static void lcd_dma_init(void)
{
    if (dma_up)
        return;
    dma_up = 1;
    /* The DMA block comes out of boot held in reset, and touching a
     * peripheral's registers while its reset is asserted stalls the AHB
     * - learned the hard way with the SPI block, now with this one. */
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~RESET_DMA);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_DMA)
            break;
    }
}

static void lcd_dma_sync(void)
{
    uint32_t spins;

    if (!dma_armed)
        return;

    /* Bounded waits: a wedged DMA or a panel that never releases BSY
     * must cost a few milliseconds at worst, never a dead kernel. */
    for (spins = 0; spins < 5000000u; spins++) {
        if (!(mmio_read32(DMA_BASE + DMA_CH_OFFSET(LCD_DMA_CH) +
                          DMA_CH_CTRL_TRIG) & DMA_CTRL_BUSY))
            break;
    }
    /* The DMA may be done while the last bytes are still in the SPI
     * FIFO / shift register.  CS must not rise over them. */
    for (spins = 0; spins < 5000000u; spins++) {
        if (!(mmio_read32(SPI1_BASE + RP_SPI_SR) & RP_SPI_SR_BSY))
            break;
    }
    for (spins = 0; spins < 64u; spins++) {
        if (!(mmio_read32(SPI1_BASE + RP_SPI_SR) & RP_SPI_SR_RNE))
            break;
        (void)mmio_read32(SPI1_BASE + RP_SPI_DR);
    }
    /* Nothing reads the panel, so the RX FIFO overruns on every byte;
     * PL022 keeps shifting with RX full (verified against pico-sdk), it
     * just leaves the sticky overrun flag set.  Clear it. */
    mmio_write32(SPI1_BASE + RP_SPI_ICR, RP_SPI_ICR_ROR);
    lcd_cs(1);
    /* Clear the flag only after the transfer is fully drained: a nested
     * caller (console output from SVC while the tick is painting) must
     * also wait, not start a second transfer on the same channel. */
    dma_armed = 0;
}

static void lcd_dma_kick(const void *buf, uint32_t n)
{
    uint32_t ch = DMA_BASE + DMA_CH_OFFSET(LCD_DMA_CH);
    uint32_t spins;

    /* Belt and braces: one channel, one buffer.  If any caller skipped a
     * sync, never program the channel while it is still walking the
     * previous transfer - two overlapping kicks scramble the panel. */
    for (spins = 0; spins < 5000000u; spins++) {
        if (!(mmio_read32(ch + DMA_CH_CTRL_TRIG) & DMA_CTRL_BUSY))
            break;
    }
    mmio_write32(ch + DMA_CH_READ_ADDR, (uint32_t)(uintptr_t)buf);
    mmio_write32(ch + DMA_CH_WRITE_ADDR, SPI1_BASE + RP_SPI_DR);
    mmio_write32(ch + DMA_CH_TRANS_COUNT, n);   /* MODE bits [31:28] = 0  */
    mmio_write32(ch + DMA_CH_CTRL_TRIG,
                 DMA_CTRL_EN | DMA_CTRL_DATA_BYTE | DMA_CTRL_INCR_READ |
                 DMA_CTRL_TREQ(DREQ_SPI1_TX));
    dma_armed = 1;
}

static void lcd_cmd(uint8_t c)
{
    lcd_dma_sync();
    lcd_dc(0);
    lcd_cs(0);
    rp_spi_write_byte(LCD_SPI_BUS, c);
    lcd_cs(1);
}

static void lcd_data(const void *buf, size_t n)
{
    if (!n)
        return;
    lcd_dma_sync();
    lcd_dc(1);
    lcd_cs(0);
    rp_spi_write(LCD_SPI_BUS, buf, n);
    lcd_cs(1);
}

/* Open a data phase for a DMA payload: CS stays low across kicks and
 * only lcd_dma_sync() closes it. */
static void lcd_data_phase(void)
{
    lcd_dma_sync();
    lcd_dc(1);
    lcd_cs(0);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *d, size_t n)
{
    lcd_cmd(cmd);
    if (n)
        lcd_data(d, n);
}

static void lcd_delay_ms(uint32_t ms)
{
    /* SysTick's counter runs from plat_timer_init() whether or not its
     * interrupt is unmasked, and the panel is initialised before
     * plat_irq_enable() - so g_ticks is still frozen here and
     * plat_uptime_ms() would never advance.  Poll the down-counter and
     * count reloads instead: one reload is exactly one tick period.
     *
     * The spin bound is a safety net, not a policy: if the timer somehow
     * is not running, boot continues with a short delay rather than
     * stopping dead on a panel that has nothing to say. */
    uint32_t last = mmio_read32(ARM_SYSTICK_VAL) & 0xFFFFFFu;
    uint32_t elapsed = 0;
    uint32_t spins = 0;

    while (elapsed < ms && spins < 20000000u) {
        uint32_t now = mmio_read32(ARM_SYSTICK_VAL) & 0xFFFFFFu;
        if (now > last)
            elapsed++;          /* wrapped: one period has gone by */
        last = now;
        spins++;
    }
}

/* ---- geometry --------------------------------------------------------- */

static void lcd_window(int x, int y, int w, int h)
{
    uint8_t b[4];

    if (w <= 0 || h <= 0)
        return;
    b[0] = (uint8_t)((x + LCD_XOFF) >> 8);
    b[1] = (uint8_t)(x + LCD_XOFF);
    b[2] = (uint8_t)((x + LCD_XOFF + w - 1) >> 8);
    b[3] = (uint8_t)(x + LCD_XOFF + w - 1);
    lcd_cmd_data(0x2a, b, 4);           /* CASET */

    b[0] = (uint8_t)((y + LCD_YOFF) >> 8);
    b[1] = (uint8_t)(y + LCD_YOFF);
    b[2] = (uint8_t)((y + LCD_YOFF + h - 1) >> 8);
    b[3] = (uint8_t)(y + LCD_YOFF + h - 1);
    lcd_cmd_data(0x2b, b, 4);           /* RASET */

    lcd_cmd(0x2c);                      /* RAMWR */
}

/* ---- init ------------------------------------------------------------- */

static void lcd_init_hw(void)
{
    static const uint8_t porch[]    = { 0x0c, 0x0c, 0x00, 0x33, 0x33 };
    static const uint8_t gctrl[]    = { 0x35 };
    static const uint8_t vcom[]     = { 0x19 };
    static const uint8_t lcmctl[]   = { 0x2c };
    static const uint8_t vdvvrhen[] = { 0x01 };
    static const uint8_t vrhs[]     = { 0x12 };
    static const uint8_t vdvs[]     = { 0x20 };
    static const uint8_t frctrl[]   = { 0x01 };
    static const uint8_t pwrctl[]   = { 0xa4, 0xa1 };
    static const uint8_t pvgam[]    = { 0xd0, 0x04, 0x0d, 0x11, 0x13, 0x2b,
                                        0x3f, 0x54, 0x4c, 0x18, 0x0d, 0x0b,
                                        0x1f, 0x23 };
    static const uint8_t nvgam[]    = { 0xd0, 0x04, 0x0c, 0x11, 0x13, 0x2c,
                                        0x3f, 0x44, 0x51, 0x2f, 0x1f, 0x1f,
                                        0x20, 0x23 };
    static const uint8_t colmod[]   = { 0x05 };   /* 16 bits per pixel */
    static const uint8_t madctl[]   = { 0x70 };   /* landscape, RGB order */

    if (lcd_up)
        return;

    lcd_dma_init();
    rp_spi_init(LCD_SPI_BUS, LCD_SPI_DIV);
    pin_func(LCD_PIN_SCK, RP_GPIO_FUNC_SPI);
    pin_func(LCD_PIN_MOSI, RP_GPIO_FUNC_SPI);
    pin_out(LCD_PIN_CS, 1);
    pin_out(LCD_PIN_DC, 1);
    pin_out(LCD_PIN_BL, 1);         /* PNP: high keeps the backlight off */
    pins_ready = 1;

    lcd_delay_ms(20);

    lcd_cmd(0x01);                      /* SWRESET */
    lcd_delay_ms(150);
    lcd_cmd(0x11);                      /* SLPOUT  */
    lcd_delay_ms(120);

    lcd_cmd_data(0x36, madctl, sizeof(madctl));
    lcd_cmd_data(0x3a, colmod, sizeof(colmod));
    lcd_cmd_data(0xb2, porch, sizeof(porch));
    lcd_cmd_data(0xb7, gctrl, sizeof(gctrl));
    lcd_cmd_data(0xbb, vcom, sizeof(vcom));
    lcd_cmd_data(0xc0, lcmctl, sizeof(lcmctl));
    lcd_cmd_data(0xc2, vdvvrhen, sizeof(vdvvrhen));
    lcd_cmd_data(0xc3, vrhs, sizeof(vrhs));
    lcd_cmd_data(0xc4, vdvs, sizeof(vdvs));
    lcd_cmd_data(0xc6, frctrl, sizeof(frctrl));
    lcd_cmd_data(0xd0, pwrctl, sizeof(pwrctl));
    lcd_cmd_data(0xe0, pvgam, sizeof(pvgam));
    lcd_cmd_data(0xe1, nvgam, sizeof(nvgam));

    lcd_cmd(0x21);                      /* INVER = on: this panel is IPS
                                         * and needs it, the vendor init
                                         * sequence turns it on too */
    lcd_cmd(0x29);                      /* DISPON */
    lcd_delay_ms(20);

    lcd_up = 1;
}

/* ---- the four hooks --------------------------------------------------- */

int plat_display_count(void)
{
    return 1;
}

/* Pixels, not cells: the text grid is whatever fits, and working it out
 * is the screen layer's job (it is the one that knows the font).  Getting
 * this backwards once cost a real debug session - the panel reported "40
 * columns" and the screen layer divided by the cell width again, leaving a
 * 6x2 terminal in the corner of the glass. */
int plat_display_info(int idx, int *w, int *h, int *cw, int *ch)
{
    if (idx != 0)
        return -1;
    lcd_init_hw();
    if (!lcd_up)
        return -1;
    if (w)
        *w = LCD_W;
    if (h)
        *h = LCD_H;
    if (cw)
        *cw = LCD_CELL_W;
    if (ch)
        *ch = LCD_CELL_H;
    return 0;
}

int plat_display_clear(int idx, uint16_t color)
{
    if (idx != 0)
        return -1;
    lcd_init_hw();
    if (!lcd_up)
        return -1;

    /* rowbuf is shared with the row hook: quiesce BEFORE refilling it,
     * or an in-flight row DMA pushes a mix of two rows to the panel. */
    lcd_dma_sync();
    /* rowbuf is free while the clear is in progress; its first line's
     * worth doubles as the repeating fill line. */
    for (int i = 0; i < LCD_W; i++) {
        rowbuf[i * 2]     = (uint8_t)(color >> 8);
        rowbuf[i * 2 + 1] = (uint8_t)color;
    }
    lcd_window(0, 0, LCD_W, LCD_H);
    lcd_data_phase();
    for (int y = 0; y < LCD_H; y++) {
        /* One buffer, so wait until the DMA has drained it before
         * refilling - but keep CS low and let the SPI FIFO catch up on
         * its own; the next kick just queues behind it. */
        uint32_t spins;
        for (spins = 0; spins < 5000000u; spins++) {
            if (!(mmio_read32(DMA_BASE + DMA_CH_OFFSET(LCD_DMA_CH) +
                              DMA_CH_CTRL_TRIG) & DMA_CTRL_BUSY))
                break;
        }
        lcd_dma_kick(rowbuf, LCD_W * 2);
    }
    lcd_dma_sync();
    return 0;
}

int plat_display_cell(int idx, int col, int row, char c,
                      uint16_t fg, uint16_t bg)
{
    const uint8_t *glyph = (const uint8_t *)0;
    unsigned ch = (unsigned char)c;
    int n = 0;

    if (idx != 0)
        return -1;
    lcd_init_hw();
    if (!lcd_up)
        return -1;
    if (col < 0 || col >= LCD_COLS || row < 0 || row >= LCD_ROWS)
        return -1;

    /* cellbuf is shared by every cell, and the previous kick may still
     * be reading it: quiesce before refilling.  The wait is ~20 us of
     * wire time, not a polled 96-byte write. */
    lcd_dma_sync();

    if (ch >= FONT5X7_FIRST && ch <= FONT5X7_LAST)
        glyph = font5x7[ch - FONT5X7_FIRST];

    for (int y = 0; y < LCD_CELL_H; y++) {
        for (int x = 0; x < LCD_CELL_W; x++) {
            int on = glyph && y < FONT5X7_H && x < FONT5X7_W &&
                     ((glyph[x] >> y) & 1u);
            uint16_t px = on ? fg : bg;
            cellbuf[n++] = (uint8_t)(px >> 8);
            cellbuf[n++] = (uint8_t)px;
        }
    }
    lcd_window(col * LCD_CELL_W, row * LCD_CELL_H, LCD_CELL_W, LCD_CELL_H);
    lcd_data_phase();
    lcd_dma_kick(cellbuf, (uint32_t)n);
    return 0;
}

/* One text row, one window, one DMA.  40 cells = 3840 bytes of pixels:
 * at 37.5 MHz that is ~0.8 ms of wire time the CPU does not wait for.
 * The caller may come back with the next row immediately; the lazy sync
 * in lcd_cmd() serialises rows. */
int plat_display_row(int idx, int row, const char *chars, int ncols,
                     uint16_t fg, uint16_t bg)
{
    int xoff = 0;
    int roww = ncols * LCD_CELL_W;      /* pixels across the painted span */

    if (idx != 0 || !chars)
        return -1;
    lcd_init_hw();
    if (!lcd_up)
        return -1;
    if (row < 0 || row >= LCD_ROWS ||
        ncols <= 0 || ncols > LCD_COLS)
        return -1;

    /* The row hook fills the shared rowbuf immediately.  A previous row's
     * DMA may still be reading that same buffer (scrolling paints 16 rows
     * back to back), so it MUST finish before the first byte changes. */
    lcd_dma_sync();

    for (int col = 0; col < ncols; col++) {
        unsigned ch = (unsigned char)chars[col];
        const uint8_t *glyph = (const uint8_t *)0;

        if (ch >= FONT5X7_FIRST && ch <= FONT5X7_LAST)
            glyph = font5x7[ch - FONT5X7_FIRST];
        for (int y = 0; y < LCD_CELL_H; y++) {
            for (int x = 0; x < LCD_CELL_W; x++) {
                int on = glyph && y < FONT5X7_H && x < FONT5X7_W &&
                         ((glyph[x] >> y) & 1u);
                uint16_t px = on ? fg : bg;
                int n = (y * roww + xoff + x) * 2;
                rowbuf[n]     = (uint8_t)(px >> 8);
                rowbuf[n + 1] = (uint8_t)px;
            }
        }
        xoff += LCD_CELL_W;
    }

    lcd_window(0, row * LCD_CELL_H, roww, LCD_CELL_H);
    lcd_data_phase();
    lcd_dma_kick(rowbuf, (uint32_t)roww * LCD_CELL_H * 2);
    return 0;
}

/* One horizontal run of RGB565 pixels (high byte first).  The buffer is
 * shared with the text hooks, so it must be quiesced BEFORE it is refilled
 * - the same rule that fixed the rolling/duplicated character rows. */
int plat_display_scan(int idx, int x, int y, int w, const uint8_t *px)
{
    if (idx != 0 || w <= 0 || w > LCD_W)
        return -1;
    lcd_init_hw();
    if (!lcd_up)
        return -1;
    if (x < 0 || y < 0 || x + w > LCD_W || y >= LCD_H)
        return -1;

    lcd_dma_sync();
    lcd_window(x, y, w, 1);
    lcd_data_phase();
    lcd_dma_kick(px, (uint32_t)w * 2u);
    return 0;
}

/* Gate for bus sharers (TF card on GPIO10/11): quiesce the queue and
 * leave CS deselected. */
void plat_display_sync(int idx)
{
    if (idx != 0)
        return;
    if (lcd_up)
        lcd_dma_sync();
}

int plat_display_backlight(int idx, int on)
{
    if (idx != 0)
        return -1;
    lcd_init_hw();
    if (!lcd_up)
        return -1;
    pin_out(LCD_PIN_BL, on ? 0 : 1);   /* active low through the PNP */
    bl_state = on ? 1 : 0;
    return 0;
}

int plat_display_backlight_get(int idx)
{
    if (idx != 0)
        return -1;
    return bl_state;
}
