#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "cpu.h"

#ifdef TOS_I2C

/* I2C0/I2C1 master, 7-bit addresses.
 *
 * The RP2040 block is a Synopsys DW_apb_i2c, not the PL011-style peripheral
 * the UARTs use.  A transfer is a short command FIFO: writing IC_DATA_CMD
 * with STOP on the last byte starts it, and the TX_ABRT bit in the raw
 * interrupt status is how a missing ACK is reported.  We poll everything,
 * with bounded loops, so a missing pull-up or an unplugged device returns
 * -1 instead of wedging a CPU.
 *
 * Pin mapping: I2C0 SDA=GP4 SCL=GP5, I2C1 SDA=GP6 SCL=GP7, FUNCSEL 3.
 * The bus needs external pull-ups; the pads enable the internal pull-up as a
 * weak fallback for a bare board. */

#define IC_CON          0x00u
#define IC_TAR          0x04u
#define IC_DATA_CMD     0x10u
#define IC_SS_SCL_HCNT  0x14u
#define IC_SS_SCL_LCNT  0x18u
#define IC_FS_SCL_HCNT  0x1cu
#define IC_FS_SCL_LCNT  0x20u
#define IC_RAW_INTR     0x34u
#define IC_CLR_INTR     0x40u
#define IC_CLR_TX_ABRT  0x54u
#define IC_ENABLE       0x6cu
#define IC_STATUS       0x70u
#define IC_SDA_HOLD     0x7cu
#define IC_ENABLE_STATUS 0x9cu

#define CON_MASTER      (1u << 0)
#define CON_SPEED_S     (1u << 1)       /* standard 100 kHz */
#define CON_SPEED_F     (2u << 1)       /* fast 400 kHz     */
#define CON_RESTART     (1u << 5)
#define CON_SLAVE_DIS   (1u << 6)
#define CON_TX_EMPTY    (1u << 8)

#define DATA_CMD_READ   (1u << 8)
#define DATA_CMD_STOP   (1u << 9)
#define DATA_CMD_RESTART (1u << 10)

#define STATUS_TFNF     (1u << 1)
#define STATUS_RFNE     (1u << 3)
#define STATUS_MST_ACT  (1u << 5)

#define RAW_TX_ABRT     (1u << 6)
#define RAW_TX_EMPTY    (1u << 4)

#define IO_BANK0_GPIO_CTRL(n) (IO_BANK0_BASE + 0x04u + 8u * (uint32_t)(n))
#define PADS_BANK0_GPIO(n)    (PADS_BANK0_BASE + 0x04u + 4u * (uint32_t)(n))

#define PAD_OD  (1u << 7)
#define PAD_IE  (1u << 6)
#define PAD_PUE (1u << 3)
#define PAD_PDE (1u << 2)
#define PAD_ISO (1u << 8)
#define FUNCSEL_I2C 3u

static const uint32_t i2c_base[2] = { I2C0_BASE, I2C1_BASE };
static const uint32_t i2c_reset[2] = { RESET_I2C0, RESET_I2C1 };
static const int i2c_pins[2][2] = {
    { 4, 5 },       /* I2C0: SDA, SCL */
    { 6, 7 },       /* I2C1: SDA, SCL */
};

static spin_t i2c_lock[2] = { SPIN_INIT, SPIN_INIT };
static int i2c_ready[2];

static void i2c_pin_init(int pin)
{
    uint32_t pad = mmio_read32(PADS_BANK0_GPIO(pin));

    pad &= ~(PAD_OD | PAD_PDE | PAD_ISO);
    pad |= PAD_IE | PAD_PUE;
    mmio_write32(PADS_BANK0_GPIO(pin), pad);
    mmio_write32(IO_BANK0_GPIO_CTRL(pin), FUNCSEL_I2C);
}

/* A few milliseconds at 125 MHz; short enough that a stuck bus cannot make
 * a 112-address scan take tens of seconds, long enough for one transfer. */
#define I2C_SPIN_LIMIT 100000u

static int i2c_wait_enabled(uint32_t base)
{
    for (uint32_t i = 0; i < I2C_SPIN_LIMIT; i++)
        if (mmio_read32(base + IC_ENABLE_STATUS) & 1u)
            return 0;
    return -1;
}

/* Disabling does not take effect until the current transfer reaches a stop
 * or is aborted.  Reconfiguring CON while the block is still enabled leaves
 * the previous transaction's state behind; repeated scans then crawl or
 * stall on an address that never completed. */
static int i2c_disable(uint32_t base)
{
    mmio_write32(base + IC_ENABLE, 0);
    for (uint32_t i = 0; i < I2C_SPIN_LIMIT; i++)
        if (!(mmio_read32(base + IC_ENABLE_STATUS) & 1u))
            return 0;
    return -1;
}

static int i2c_init_once(int bus)
{
    uint32_t base = i2c_base[bus], v;

    if (i2c_ready[bus])
        return 0;

    v = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    if (v & i2c_reset[bus]) {
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                     v & ~i2c_reset[bus]);
        for (uint32_t i = 0; i < I2C_SPIN_LIMIT; i++)
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
                i2c_reset[bus])
                break;
    }

    i2c_pin_init(i2c_pins[bus][0]);
    i2c_pin_init(i2c_pins[bus][1]);

    if (i2c_disable(base) < 0)
        return -1;
    (void)mmio_read32(base + IC_CLR_INTR);
    mmio_write32(base + IC_ENABLE, 1);
    if (i2c_wait_enabled(base) < 0)
        return -1;
    i2c_ready[bus] = 1;
    return 0;
}

static int i2c_apply(int bus, int freq_hz)
{
    uint32_t base = i2c_base[bus];
    uint32_t period, hcnt, lcnt, con;

    if (freq_hz <= 0 || freq_hz > 1000000)
        return -1;

    period = (RP2040_SYS_CLK_HZ + (uint32_t)freq_hz / 2u) /
             (uint32_t)freq_hz;
    hcnt = period * 3u / 5u;
    lcnt = period - hcnt;
    if (hcnt < 8u)
        hcnt = 8u;
    if (lcnt < 8u)
        lcnt = 8u;
    if (hcnt > 0xffffu || lcnt > 0xffffu)
        return -1;

    con = CON_MASTER | CON_RESTART | CON_SLAVE_DIS | CON_TX_EMPTY;
    con |= (freq_hz <= 100000) ? CON_SPEED_S : CON_SPEED_F;

    if (i2c_disable(base) < 0)
        return -1;
    (void)mmio_read32(base + IC_CLR_INTR);
    (void)mmio_read32(base + IC_CLR_TX_ABRT);
    mmio_write32(base + IC_SS_SCL_HCNT, hcnt);
    mmio_write32(base + IC_SS_SCL_LCNT, lcnt);
    mmio_write32(base + IC_FS_SCL_HCNT, hcnt);
    mmio_write32(base + IC_FS_SCL_LCNT, lcnt);
    mmio_write32(base + IC_SDA_HOLD, 38);
    mmio_write32(base + IC_CON, con);
    mmio_write32(base + IC_ENABLE, 1);
    return i2c_wait_enabled(base);
}

static int i2c_aborted(uint32_t base)
{
    uint32_t raw = mmio_read32(base + IC_RAW_INTR);

    if (raw & RAW_TX_ABRT) {
        (void)mmio_read32(base + IC_CLR_TX_ABRT);
        return 1;
    }
    return 0;
}

/* Every wait also watches TX_ABRT: an absent device raises it immediately,
 * so waiting out a full timeout per address would make a 112-address scan
 * take seconds instead of milliseconds. */
static int i2c_wait_tfnf(uint32_t base)
{
    for (uint32_t i = 0; i < I2C_SPIN_LIMIT; i++) {
        uint32_t raw = mmio_read32(base + IC_RAW_INTR);
        if (raw & RAW_TX_ABRT)
            return -1;
        if (mmio_read32(base + IC_STATUS) & STATUS_TFNF)
            return 0;
    }
    return -1;
}

static int i2c_wait_rfne(uint32_t base)
{
    for (uint32_t i = 0; i < I2C_SPIN_LIMIT; i++) {
        uint32_t raw = mmio_read32(base + IC_RAW_INTR);
        if (raw & RAW_TX_ABRT)
            return -1;
        if (mmio_read32(base + IC_STATUS) & STATUS_RFNE)
            return 0;
    }
    return -1;
}

static int i2c_wait_tx_empty(uint32_t base)
{
    for (uint32_t i = 0; i < I2C_SPIN_LIMIT; i++) {
        uint32_t raw = mmio_read32(base + IC_RAW_INTR);
        if (raw & RAW_TX_ABRT)
            return -1;
        if (raw & RAW_TX_EMPTY)
            return 0;
    }
    return -1;
}

static int i2c_wait_idle(uint32_t base)
{
    for (uint32_t i = 0; i < I2C_SPIN_LIMIT; i++) {
        uint32_t raw = mmio_read32(base + IC_RAW_INTR);
        if (raw & RAW_TX_ABRT)
            return -1;
        if (!(mmio_read32(base + IC_STATUS) & STATUS_MST_ACT))
            return 0;
    }
    return -1;
}

int plat_i2c_count(void)
{
    return 2;
}

int plat_i2c_xfer(int bus, int addr, int freq_hz,
                  const void *tx, int txlen, void *rx, int rxlen)
{
    const uint8_t *src = (const uint8_t *)tx;
    uint8_t *dst = (uint8_t *)rx;
    uint32_t base;

    if (bus < 0 || bus >= 2 || addr < 0 || addr > 0x7f ||
        txlen < 0 || rxlen < 0)
        return -1;
    if ((!src && txlen > 0) || (!dst && rxlen > 0))
        return -1;

    spin_lock(&i2c_lock[bus]);
    if (i2c_init_once(bus) < 0 || i2c_apply(bus, freq_hz) < 0) {
        spin_unlock(&i2c_lock[bus]);
        return -1;
    }
    base = i2c_base[bus];

    mmio_write32(base + IC_TAR, (uint32_t)addr);

    for (int i = 0; i < txlen; i++) {
        if (i2c_wait_tfnf(base) < 0)
            goto fail;
        uint32_t cmd = src[i];
        if (i == txlen - 1 && rxlen == 0)
            cmd |= DATA_CMD_STOP;
        mmio_write32(base + IC_DATA_CMD, cmd);
        if (i2c_wait_tx_empty(base) < 0 || i2c_aborted(base))
            goto fail;
    }

    for (int i = 0; i < rxlen; i++) {
        if (i2c_wait_tfnf(base) < 0)
            goto fail;
        uint32_t cmd = DATA_CMD_READ;
        if (i == 0 && txlen > 0)
            cmd |= DATA_CMD_RESTART;
        if (i == rxlen - 1)
            cmd |= DATA_CMD_STOP;
        mmio_write32(base + IC_DATA_CMD, cmd);
        if (i2c_wait_rfne(base) < 0 || i2c_aborted(base))
            goto fail;
        dst[i] = (uint8_t)(mmio_read32(base + IC_DATA_CMD) & 0xFFu);
    }

    if (i2c_wait_idle(base) < 0)
        goto fail;

    spin_unlock(&i2c_lock[bus]);
    return txlen + rxlen;

fail:
    (void)mmio_read32(base + IC_CLR_TX_ABRT);
    spin_unlock(&i2c_lock[bus]);
    return -1;
}

#endif /* TOS_I2C */
