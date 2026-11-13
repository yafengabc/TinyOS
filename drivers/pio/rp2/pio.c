#include "platform.h"
#include "board.h"
#include "string.h"

#ifdef TOS_PIO

/* PIO loader for the RP2040 and the RP2350.
 *
 * RP2040: two blocks, RP2350: three; four state machines and 32 instruction
 * words per block either way, and the register map is byte-for-byte the same
 * (pico-sdk's hardware/regs/pio.h has identical offsets for both parts).  What
 * moved is the surrounding land: RESETS and IO_BANK0 live at different base
 * addresses, PIO0/PIO1 sit on different RESETS bits (10/11 vs 11/12), the
 * RP2350 adds a PIO2 on bit 13 / FUNCSEL 8, and every RP2350 pad comes out of
 * reset with ISO asserted.  Those constants come from the board's memmap
 * header (RP_PIO_*), so one driver covers all three boards.
 *
 * The assembler lives in user space (user/pio.c); the kernel only takes a word
 * array plus the configuration that is *not* encoded in the words: wrap
 * addresses, side-set width/flags, clock divider, pin base and the shift
 * control flags.
 *
 * Ownership matters because PIO is system hardware: a state machine loaded by
 * a task is disabled and its pins handed back to the GPIO block when that task
 * exits (plat_pio_release).  Otherwise a program that loads a PIO program and
 * returns would leave GP0/GP1 muxed away from the console UART.
 */

#if defined(__arm__)
#include "arm_memmap.h"        /* board/rp2040 or board/rp2350, per target */
#else
#include "rp2350rv_memmap.h"   /* the only RISC-V board with PIO            */
#endif

#define PIO_CTRL           0x000u
#define PIO_FSTAT          0x004u
#define PIO_FDEBUG         0x008u
#define PIO_TXF(n)         (0x010u + 4u * (uint32_t)(n))
#define PIO_RXF(n)         (0x020u + 4u * (uint32_t)(n))
#define PIO_INSTR_MEM(i)   (0x048u + 4u * (uint32_t)(i))

#define PIO_SM(n, off)     (0x0c8u + 0x18u * (uint32_t)(n) + (off))
#define SM_CLKDIV          0x00u
#define SM_EXECCTRL        0x04u
#define SM_SHIFTCTRL       0x08u
#define SM_ADDR            0x0cu
#define SM_INSTR           0x10u
#define SM_PINCTRL         0x14u

#define CTRL_ENABLE(sm)    (1u << (sm))
#define CTRL_RESTART(sm)   (1u << (4 + (sm)))

#define FSTAT_TXFULL(sm)   (1u << (16 + (sm)))
#define FSTAT_RXEMPTY(sm)  (1u << (8 + (sm)))

#define EXEC_SIDE_EN       (1u << 30)   /* `.side_set N opt`            */
#define EXEC_SIDE_PINDIR   (1u << 29)

#define PINCTRL_SIDESET_CNT(n)  ((uint32_t)(n) << 29)
#define PINCTRL_SET_CNT(n)      ((uint32_t)(n) << 26)
#define PINCTRL_OUT_CNT(n)      ((uint32_t)(n) << 20)
#define PINCTRL_IN_BASE(p)      ((uint32_t)(p) << 15)
#define PINCTRL_SIDESET_BASE(p) ((uint32_t)(p) << 10)
#define PINCTRL_SET_BASE(p)     ((uint32_t)(p) << 5)
#define PINCTRL_OUT_BASE(p)     ((uint32_t)(p))

/* pid that owns each SM, 0 when free; the pin window to hand back on release. */
static int32_t pio_owner[RP_PIO_BLOCKS][4];
static int8_t  pio_pinbase[RP_PIO_BLOCKS][4];
static uint8_t pio_pincount[RP_PIO_BLOCKS][4];
static int     pio_inited;

static int block_ok(int b)
{
    return b >= 0 && b < RP_PIO_BLOCKS;
}

static void pio_init_once(void)
{
    uint32_t want = 0;

    if (pio_inited)
        return;
    for (int b = 0; b < RP_PIO_BLOCKS; b++)
        want |= RP_PIO_RESET_BIT(b);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~want);
    for (uint32_t i = 0; i < 1000000u; i++)
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & want) == want)
            break;
    pio_inited = 1;
}

static void pin_to_pio(int block, int pin)
{
    uint32_t ctrl = RP_GPIO_CTRL(pin);
    uint32_t pad  = RP_GPIO_PAD(pin);

    mmio_write32(ctrl, RP_PIO_FUNCSEL(block));
    /* Input enable on (IN pins and readback), open drain off, ISO off.
     * Bit 8 is the isolation bit: reserved on the RP2040, and 1 out of
     * reset on every RP2350 pad, so it has to be cleared on both. */
    mmio_write32(pad, (mmio_read32(pad) | (1u << 6)) &
                      ~((1u << 7) | (1u << 8)));
}

static void pin_release(int pin)
{
    /* Back to the GPIO/SIO function, input for now. */
    plat_gpio_configure(pin, PLAT_GPIO_IN);
}

static void stop_sm(int block, int sm)
{
    uint32_t base = RP_PIO_BASE(block);

    mmio_write32(base + PIO_CTRL,
                 mmio_read32(base + PIO_CTRL) & ~CTRL_ENABLE(sm));
}

static void release_sm(int block, int sm)
{
    int base_pin = pio_pinbase[block][sm];
    int count = pio_pincount[block][sm];

    stop_sm(block, sm);
    if (base_pin >= 0)
        for (int i = 0; i < count; i++)
            pin_release(base_pin + i);
    pio_owner[block][sm] = 0;
    pio_pinbase[block][sm] = -1;
    pio_pincount[block][sm] = 0;
}

void plat_pio_release(int pid)
{
    if (!pio_inited)
        return;
    for (int b = 0; b < RP_PIO_BLOCKS; b++)
        for (int s = 0; s < 4; s++)
            if (pio_owner[b][s] == pid)
                release_sm(b, s);
}

static int pio_load(struct pio_req *q, int pid)
{
    int b = q->block, sm = q->sm;
    uint32_t base, top, bottom, exec, pinctrl;
    int pins = plat_gpio_count();

    if (!block_ok(b) || sm < 0 || sm > 3)
        return -1;
    if (q->len < 1 || q->len > 32 || q->origin < 0 ||
        q->origin + q->len > 32)
        return -1;
    if (q->side_set < 0 || q->side_set > 5 || q->pin_count < 0 ||
        q->pin_count > 32)
        return -1;
    if (!q->buf)
        return -1;

    top = q->wrap_top;
    bottom = q->wrap_bottom;
    if (top < 0)
        top = q->origin + q->len - 1;
    if (bottom < 0)
        bottom = q->origin;
    if (top < q->origin || top >= q->origin + q->len ||
        bottom < q->origin || bottom >= q->origin + q->len || bottom > top)
        return -1;

    /* The pin window is checked before anything is stopped or muxed, so a bad
     * request cannot leave a state machine half configured. */
    if (q->pin_base >= 0) {
        if (pins <= 0 || q->pin_base + q->pin_count > pins)
            return -1;
    }

    pio_init_once();
    if (pio_owner[b][sm] && pio_owner[b][sm] != pid)
        return -1;

    base = RP_PIO_BASE(b);

    /* Stop, clear internal state and both FIFOs, then load the words. */
    stop_sm(b, sm);
    mmio_write32(base + PIO_CTRL,
                 mmio_read32(base + PIO_CTRL) | CTRL_RESTART(sm));
    mmio_write32(base + PIO_FDEBUG, 0x0f0f0f0fu);

    {
        const uint16_t *w = (const uint16_t *)q->buf;

        for (int i = 0; i < q->len; i++)
            mmio_write32(base + PIO_INSTR_MEM(q->origin + i), w[i]);
    }

    /* Clock divider: INT [31:16], FRAC [15:8]; 0 would mean /65536. */
    {
        uint32_t div = ((uint32_t)(q->clkdiv_int & 0xffff) << 16) |
                       ((uint32_t)(q->clkdiv_frac & 0xff) << 8);

        mmio_write32(base + PIO_SM(sm, SM_CLKDIV), div ? div : (1u << 16));
    }

    exec = (top << 12) | (bottom << 7);
    /* Optional side-set is an encoding detail: with `opt` the top side-set bit
     * means "drive the pins this cycle".  EXECCTRL only needs to know that the
     * delay field carries side-set bits at all, so the flag is not consulted. */
    if (q->side_set > 0)
        exec |= EXEC_SIDE_EN;
    if (q->flags & PIO_F_SIDE_PINDIR)
        exec |= EXEC_SIDE_PINDIR;
    mmio_write32(base + PIO_SM(sm, SM_EXECCTRL), exec);

    mmio_write32(base + PIO_SM(sm, SM_SHIFTCTRL), (uint32_t)q->shiftctrl);

    pinctrl = PINCTRL_SIDESET_CNT(q->side_set);
    if (q->pin_base >= 0) {
        pinctrl |= PINCTRL_OUT_CNT(q->pin_count & 0x3f);
        pinctrl |= PINCTRL_SET_CNT((q->pin_count > 5 ? 5 : q->pin_count));
        pinctrl |= PINCTRL_IN_BASE(q->pin_base);
        pinctrl |= PINCTRL_SIDESET_BASE(q->pin_base);
        pinctrl |= PINCTRL_SET_BASE(q->pin_base);
        pinctrl |= PINCTRL_OUT_BASE(q->pin_base);
        for (int i = 0; i < q->pin_count; i++)
            pin_to_pio(b, q->pin_base + i);
    }
    mmio_write32(base + PIO_SM(sm, SM_PINCTRL), pinctrl);

    pio_owner[b][sm] = pid;
    pio_pinbase[b][sm] = (int8_t)q->pin_base;
    pio_pincount[b][sm] = (uint8_t)q->pin_count;
    return 0;
}

static int pio_tx(struct pio_req *q)
{
    uint32_t base;
    const uint32_t *w = (const uint32_t *)q->buf;
    int n = 0;

    if (!block_ok(q->block) || q->sm < 0 || q->sm > 3 ||
        !q->buf || q->len < 0)
        return -1;
    base = RP_PIO_BASE(q->block);
    while (n < q->len) {
        if (mmio_read32(base + PIO_FSTAT) & FSTAT_TXFULL(q->sm))
            break;
        mmio_write32(base + PIO_TXF(q->sm), w[n]);
        n++;
    }
    return n;
}

static int pio_rx(struct pio_req *q)
{
    uint32_t base;
    uint32_t *w = (uint32_t *)q->buf;
    int n = 0;

    if (!block_ok(q->block) || q->sm < 0 || q->sm > 3 ||
        !q->buf || q->len < 0)
        return -1;
    base = RP_PIO_BASE(q->block);
    while (n < q->len) {
        if (mmio_read32(base + PIO_FSTAT) & FSTAT_RXEMPTY(q->sm))
            break;
        w[n++] = mmio_read32(base + PIO_RXF(q->sm));
    }
    return n;
}

int plat_pio(int op, struct pio_req *req, int pid)
{
    uint32_t base;

    if (!req)
        return -1;

    switch (op) {
    case PIO_LOAD:
        return pio_load(req, pid);

    case PIO_START:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (pio_owner[req->block][req->sm] != pid)
            return -1;
        base = RP_PIO_BASE(req->block);
        mmio_write32(base + PIO_CTRL,
                     mmio_read32(base + PIO_CTRL) | CTRL_ENABLE(req->sm));
        return 0;

    case PIO_STOP:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (pio_owner[req->block][req->sm] != pid)
            return -1;
        stop_sm(req->block, req->sm);
        return 0;

    case PIO_TX:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (pio_owner[req->block][req->sm] != pid)
            return -1;
        return pio_tx(req);

    case PIO_RX:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (pio_owner[req->block][req->sm] != pid)
            return -1;
        return pio_rx(req);

    case PIO_RELEASE:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (pio_owner[req->block][req->sm] != pid)
            return -1;
        release_sm(req->block, req->sm);
        return 0;
    }
    return -1;
}

#endif /* TOS_PIO */
