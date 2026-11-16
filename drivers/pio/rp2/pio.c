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
#define PIO_FLEVEL         0x00cu
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

/* SMx_SHIFTCTRL: FJOIN_RX [15:14] (bits 13/14 shared with FJOIN_TX), i.e.
 * 1 = both FIFOs as TX, 2 = both as RX, 3 = one 8-deep RX.  Toggling bit 14
 * is the flush (see clear_fifos). */
#define SHIFTCTRL_FJOIN_RX RP_PIO_FJOIN_RX_BIT

/* SHIFTCTRL's reset value is 0x000c0000: IN_SHIFTDIR and OUT_SHIFTDIR both
 * mean "shift right".  A driver that programs SHIFTCTRL with the caller's
 * bits alone therefore flips both shifters to shift *left*, and `out pins`
 * starts emitting the MSB of the word instead of the LSB - which looks like
 * a dead pin: four words of 0x00000001 came back as 00000000 and only an
 * all-ones pattern (0xffffffff) echoed correctly (measured on the RP2350).
 * Keep the two direction bits unless the caller asks otherwise. */
#define SHIFTCTRL_RIGHT_BOTH 0x000c0000u

/* SMx_EXECCTRL bit layout, from pico-sdk's hardware/regs/pio.h (identical on
 * the RP2040 and the RP2350): WRAP_TOP [16:12], WRAP_BOTTOM [11:7],
 * SIDE_EN bit 30, SIDE_PINDIR bit 29.
 *
 * This got "fixed" the wrong way once already.  The fields looked like they
 * belonged at [11:7]/[4:0] the way PINCTRL's do, so a rebuild moved them
 * there - and every PIO program on the board then ran in a wrap window
 * nobody had asked for: `pio start` reported ok, the machine sat on an
 * address past the end of the program, `tx` filled a FIFO nothing drained
 * and the pin never moved.  Check the register header before touching these,
 * not the neighbouring register's layout. */
#define EXEC_WRAP_TOP(t)    ((uint32_t)((t) & 0x1fu) << 12)
#define EXEC_WRAP_BOTTOM(b) ((uint32_t)((b) & 0x1fu) << 7)
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

/* PIO hangs off the RP2350 fast bus, where a store is acknowledged before it
 * reaches the peripheral; two stores from the same function are not ordered
 * with respect to each other without a barrier.  This bit the driver hard:
 * the jmp that sets the PC could land *before* the CTRL.ENABLE next to it,
 * so the machine was enabled on top of a PC nobody had set yet.  pico-sdk
 * gets away with compiler barriers only because its stores are spaced out. */
static void pio_fence(void)
{
#if defined(__arm__)
    __asm__ volatile ("dsb" ::: "memory");
#else
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
#endif
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
    for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++)
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

/* Empty both FIFOs.
 *
 * SM_RESTART resets the shift registers and the PC, but the four words of
 * FIFO storage survive it, so a reload inherits whatever the previous user
 * left behind.  That is what made `pio tx` answer "wrote 0 word(s)" on a
 * freshly loaded machine (measured on the RP2350): the TX FIFO still held
 * four stale words from an earlier program, and FSTAT reported it full.
 * Toggling FJOIN_RX twice is the documented flush - changing the join state
 * reallocates the FIFO storage - and it is what pico-sdk's
 * pio_sm_clear_fifos() does. */
static void clear_fifos(int block, int sm)
{
    uint32_t base = RP_PIO_BASE(block);
    uint32_t sc = mmio_read32(base + PIO_SM(sm, SM_SHIFTCTRL));

    mmio_write32(base + PIO_SM(sm, SM_SHIFTCTRL), sc ^ SHIFTCTRL_FJOIN_RX);
    mmio_write32(base + PIO_SM(sm, SM_SHIFTCTRL), sc);
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

/* Called when a process exits.  It gives up ownership but deliberately does
 * NOT stop the machine or hand the pins back.
 *
 * The shell runs every `pio ...` verb as its own process, so stopping on exit
 * made the whole tool useless: `pio start` set CTRL.ENABLE, the process
 * exited, this function cleared it again, and the following `pio tx` filled a
 * FIFO nothing was draining - `pio rx` then read zero words (measured on the
 * RP2350 with a fresh boot: start ok, wrote 4, read 0).  A machine that keeps
 * running is also the point of `blink`: the LED is supposed to keep blinking
 * after the command that started it has returned.
 *
 * Releasing the hardware stays explicit (`pio rel`), which stops the machine
 * and returns the pins to GPIO.  Ownership is cleared here so the next
 * command can claim the machine; a machine whose owner died without calling
 * rel keeps running until somebody does. */
void plat_pio_release(int pid)
{
    if (!pio_inited)
        return;
    for (int b = 0; b < RP_PIO_BLOCKS; b++)
        for (int s = 0; s < 4; s++)
            if (pio_owner[b][s] == pid)
                pio_owner[b][s] = 0;
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

    /* Same order pico-sdk's pio_sm_init() uses: stop, flush the FIFOs through
     * SHIFTCTRL, restart, then program the configuration.  Doing the restart
     * first and the flush second left the machine holding FIFO state that
     * disagreed with FSTAT (measured on the RP2350: `tx` reported four words
     * queued, FLEVEL agreed, and the machine's pull never saw them). */
    stop_sm(b, sm);
    clear_fifos(b, sm);
    mmio_write32(base + PIO_CTRL,
                 mmio_read32(base + PIO_CTRL) | CTRL_RESTART(sm));
    mmio_write32(base + PIO_FDEBUG, 0x0f0f0f0fu);
    pio_fence();

    {
        const uint16_t *w = (const uint16_t *)q->buf;

        for (int i = 0; i < q->len; i++)
            mmio_write32(base + PIO_INSTR_MEM(q->origin + i), w[i]);
    }
    pio_fence();

    /* Clock divider: INT [31:16], FRAC [15:8]; 0 would mean /65536. */
    {
        uint32_t div = ((uint32_t)(q->clkdiv_int & 0xffff) << 16) |
                       ((uint32_t)(q->clkdiv_frac & 0xff) << 8);

        mmio_write32(base + PIO_SM(sm, SM_CLKDIV), div ? div : (1u << 16));
    }

    exec = EXEC_WRAP_TOP(top) | EXEC_WRAP_BOTTOM(bottom);
    /* Optional side-set is an encoding detail: with `opt` the top side-set bit
     * means "drive the pins this cycle".  EXECCTRL only needs to know that the
     * delay field carries side-set bits at all, so the flag is not consulted. */
    if (q->side_set > 0)
        exec |= EXEC_SIDE_EN;
    if (q->flags & PIO_F_SIDE_PINDIR)
        exec |= EXEC_SIDE_PINDIR;
    mmio_write32(base + PIO_SM(sm, SM_EXECCTRL), exec);

    mmio_write32(base + PIO_SM(sm, SM_SHIFTCTRL),
                 (uint32_t)q->shiftctrl | SHIFTCTRL_RIGHT_BOTH);

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

/* Take ownership of a state machine for this call, or refuse.
 *
 * The claim does not have to be made by the process that loaded the program.
 * The shell runs every `pio ...` verb as its own process, so `pio asm` and
 * `pio start` are never the same pid; requiring equality made the tool
 * unusable - asm reported `loaded into pio0 sm0` and every later verb
 * answered `failed (not owner, or no PIO)` (measured on the RP2350).  An
 * unowned SM is therefore claimable, exactly as pio_load() already treats it,
 * while a live owner still keeps a second process out until it exits
 * (plat_pio_release) or releases explicitly. */
static int claim_sm(int block, int sm, int pid)
{
    if (pio_owner[block][sm] && pio_owner[block][sm] != pid)
        return -1;
    pio_owner[block][sm] = pid;
    return 0;
}

/* Read back what the hardware holds.  Diagnostic only, but the loader's
 * intent and the register contents are not the same thing: a wrong field
 * offset writes a plausible-looking value somewhere else, and no amount of
 * reading the driver tells you which.  Eight words: CTRL, FSTAT, EXECCTRL,
 * SHIFTCTRL, PINCTRL, CLKDIV, ADDR, owner pid. */
static int pio_state(struct pio_req *q)
{
    uint32_t *w = (uint32_t *)q->buf;
    uint32_t base;
    int base_pin = pio_pinbase[q->block][q->sm];

    if (!w || q->len < PIO_STATE_WORDS)
        return -1;
    base = RP_PIO_BASE(q->block);
    w[0] = mmio_read32(base + PIO_CTRL);
    w[1] = mmio_read32(base + PIO_FSTAT);
    w[2] = mmio_read32(base + PIO_SM(q->sm, SM_EXECCTRL));
    w[3] = mmio_read32(base + PIO_SM(q->sm, SM_SHIFTCTRL));
    w[4] = mmio_read32(base + PIO_SM(q->sm, SM_PINCTRL));
    w[5] = mmio_read32(base + PIO_SM(q->sm, SM_CLKDIV));
    w[6] = mmio_read32(base + PIO_SM(q->sm, SM_ADDR));
    w[7] = (uint32_t)pio_owner[q->block][q->sm];   /* 0 = unowned */
    /* The pin side, because half of every PIO question is "did the pad move
     * and did anything read it": FUNCSEL, IE/OD/ISO and the FIFO levels
     * answer that without a scope.  No instruction-memory dump: INSTR_MEM0..31
     * are write-only on both parts, so a read returns zero and looks exactly
     * like a program that failed to load.  `pio exec` probes the program
     * instead. */
    w[8] = base_pin >= 0 ? mmio_read32(RP_GPIO_CTRL(base_pin)) : 0xffffffffu;
    w[9] = base_pin >= 0 ? mmio_read32(RP_GPIO_PAD(base_pin)) : 0xffffffffu;
    w[10] = mmio_read32(base + PIO_FLEVEL);
    return PIO_STATE_WORDS;
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
        if (claim_sm(req->block, req->sm, pid) < 0)
            return -1;
        base = RP_PIO_BASE(req->block);
        mmio_write32(base + PIO_CTRL,
                     mmio_read32(base + PIO_CTRL) | CTRL_ENABLE(req->sm));
        pio_fence();
        return 0;

    case PIO_STOP:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (claim_sm(req->block, req->sm, pid) < 0)
            return -1;
        stop_sm(req->block, req->sm);
        return 0;

    case PIO_TX:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (claim_sm(req->block, req->sm, pid) < 0)
            return -1;
        return pio_tx(req);

    case PIO_RX:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (claim_sm(req->block, req->sm, pid) < 0)
            return -1;
        return pio_rx(req);

    case PIO_RELEASE:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        if (claim_sm(req->block, req->sm, pid) < 0)
            return -1;
        release_sm(req->block, req->sm);
        return 0;

    case PIO_STATE:
        /* No claim: reading registers must be possible even when another
         * process owns the machine, which is exactly when it is interesting. */
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        return pio_state(req);

    case PIO_EXEC:
        if (!block_ok(req->block) || req->sm < 0 || req->sm > 3)
            return -1;
        /* Writing SMx_INSTR hands the instruction to the machine.  With the
         * machine running the write is taken at the next instruction
         * boundary; with it disabled it takes effect too - which is how
         * `jmp 0` moves a PC that SM_RESTART did not put where we wanted.
         * If the machine is stalled on a pull or a push the write blocks,
         * so it is not something to fire blindly at a live machine. */
        base = RP_PIO_BASE(req->block);
        mmio_write32(base + PIO_SM(req->sm, SM_INSTR),
                     (uint32_t)req->len & 0xffffu);
        pio_fence();
        return 0;
    }
    return -1;
}

#endif /* TOS_PIO */
