#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "console.h"
#include "string.h"

/* Board: QEMU mps2-an385 (Cortex-M3).
 *
 * CMSDK UART0 for the console, SysTick for the tick, a RAM disk backing
 * the littlefs partition, and the CMSDK dual-timer/test registers for
 * shutdown.  mps2-an385 has no flash controller: code runs from the image
 * the loader placed at 0. */

/* ---- CMSDK UART0 ------------------------------------------------------ */

/*  DATA  0x00  write: TXD; read: RXD
 *  STATE 0x04  bit0 TX full, bit1 RX full
 *  CTRL  0x08  bit0 TX enable, bit1 RX enable
 *  BAUD  0x10  divisor (QEMU ignores it)
 *  INTSTATUS 0x1C, INTCLR 0x20: interrupts are polled, never enabled */

void plat_console_init(void)
{
    mmio_write32(ARM_UART0_BAUD, 16);
    mmio_write32(ARM_UART0_CTRL, 3);      /* TX + RX enable               */
}

void plat_console_putc(char c)
{
    while (mmio_read32(ARM_UART0_STATE) & 1u)
        ;                                  /* wait for the TX FIFO         */
    mmio_write32(ARM_UART0_DATA, (uint32_t)(uint8_t)c);
}

void plat_console_flush(void)
{
    while (mmio_read32(ARM_UART0_STATE) & 1u)
        ;                                  /* FIFO empty: the last byte is
                                              on its way to the host */
}

int plat_console_getchar(void)
{
    if (!(mmio_read32(ARM_UART0_STATE) & 2u))
        return -1;
    return (int)(mmio_read32(ARM_UART0_DATA) & 0xFFu);
}

/* CMSDK UART0 keeps a single-byte holding register: either it is free or it
 * is not.  Echo asks first so it never spins the tick away. */
int plat_console_tx_room(void)
{
    return (mmio_read32(ARM_UART0_STATE) & 1u) ? 0 : 1;
}

/* ---- SysTick ---------------------------------------------------------- */

/* The CMSDK in mps2-an385 runs SysTick from the 25 MHz reference clock.  A
 * reload value of CLK/1000 gives 1 ms granularity; the kernel asks for
 * plat_ticks() to advance at `hz`, so the reload is CLK/hz. */

static volatile uint64_t g_ticks;
static uint32_t g_reload;

void plat_timer_init(uint32_t hz)
{
    g_reload = ARM_SYSTICK_CLK / hz;
    if (g_reload == 0 || g_reload > 0xFFFFFFu)
        g_reload = 0xFFFFFFu;
    g_ticks = 0;

    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    /* Start the counter but leave the interrupt masked: kmain calls this
       before the scheduler is up, and on a Cortex-M enabling TICKINT here
       would take a tick straight into process_tick() with an initialised
       guard word of zero.  plat_irq_enable() turns it on later, which is
       the same ordering the rv32 port gets from mstatus.MIE. */
    mmio_write32(ARM_SYSTICK_CTRL, 5u);
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return ARM_SYSTICK_CLK / g_reload;
}

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)((g_ticks * 1000u) / (uint64_t)plat_tick_hz());
}

/* Clock: whole periods from the tick counter, position inside the current
 * one from SysTick's live down-counter.  That is 1/25 MHz of resolution
 * instead of one tick.  Reading VAL while the counter wraps would count a
 * period twice, so the answer is clamped to stay monotonic. */
uint64_t plat_now_ns(void)
{
    static uint64_t last_ns;
    uint32_t load = g_reload;
    uint32_t val  = mmio_read32(ARM_SYSTICK_VAL) & 0xFFFFFFu;
    uint32_t into = (load > val) ? (load - val) : 0u;
    uint64_t cycles = g_ticks * (uint64_t)load + into;
    uint64_t per_us = ARM_SYSTICK_CLK / 1000000u;     /* cycles per microsecond */
    uint64_t ns = (cycles / per_us) * 1000ull
                + ((cycles % per_us) * 1000ull) / per_us;

    if (ns < last_ns)
        ns = last_ns;
    last_ns = ns;
    return ns;
}

void plat_timer_tick(void)
{
    g_ticks++;
    /* SysTick reloads itself; nothing to re-arm. */
}

/* ---- block device: RAM disk ------------------------------------------ */

/* The build tool links the littlefs image into the kernel (.data.ramdisk),
 * so the partition is a writable buffer in RAM and starts at block 0. */

extern uint8_t __ramdisk_start[];
extern uint8_t __ramdisk_end[];

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    uint32_t off = lba * 512u;
    uint32_t len = count * 512u;
    if (lba != 0 && off < lba)
        return -1;
    if (off + len < off || off + len > (uint32_t)(__ramdisk_end - __ramdisk_start))
        return -1;
    memcpy(buf, __ramdisk_start + off, len);
    return 0;
}

int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    uint32_t off = lba * 512u;
    uint32_t len = count * 512u;
    if (lba != 0 && off < lba)
        return -1;
    if (off + len < off || off + len > (uint32_t)(__ramdisk_end - __ramdisk_start))
        return -1;
    memcpy(__ramdisk_start + off, buf, len);
    return 0;
}

int plat_block_erase(uint32_t lba, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t off = (lba + i) * 512u;
        if (off + 512u > (uint32_t)(__ramdisk_end - __ramdisk_start))
            return -1;
        memset(__ramdisk_start + off, 0xFF, 512);
    }
    return 0;
}

uint32_t plat_block_size(void)
{
    return 512;
}

uint32_t plat_block_count(void)
{
    return (uint32_t)(__ramdisk_end - __ramdisk_start) / 512u;
}

/* ---- boot ------------------------------------------------------------ */

void plat_board_early(void)
{
    plat_console_init();
}

void plat_board_init(void)
{
    /* Flush any SysTick condition the loader may have left behind; the
       counter itself is started by plat_timer_init. */
    mmio_write32(ARM_SYSTICK_VAL, 0);
}

/* Called by plat_irq_enable() the first time interrupts come on.  TICKINT
 * stays clear until then so a tick cannot arrive before the scheduler
 * exists (see the comment in plat_timer_init). */
void plat_timer_irq_unmask(void)
{
    uint32_t ctrl = mmio_read32(ARM_SYSTICK_CTRL);

    if (!(ctrl & 2u))
        mmio_write32(ARM_SYSTICK_CTRL, ctrl | 2u);

}

/* ---- power ------------------------------------------------------------ */

/* CMSDK dual-timer at 0x40002000 in some mps2 variants; an385 exposes the
 * FPGA's "control" registers instead.  QEMU terminates the guest when the
 * ARM semihosting SYS_EXIT would run, so shutdown lowers to a halt after
 * flushing the console - the smoke test treats the final marker as the
 * end of the run. */
void plat_shutdown(void)
{
    console_shutdown();
    plat_panic_halt();
}

void plat_reboot(void)
{
    console_shutdown();

    /* Same core-level reset as the RP2040 port.  QEMU's mps2-an385 accepts
     * SYSRESETREQ; with no flash controller the machine re-runs the image
     * the loader put at 0. */
    mmio_write32(ARM_AIRCR, ARM_AIRCR_VECTKEY | ARM_AIRCR_SYSRESETREQ);

    plat_panic_halt();
}



/* Single-core M3: there is no core id register to read. */
uint32_t plat_board_cpu_id(void)
{
    return 0;
}

const char *plat_cpu_model(void)
{
    return "Cortex-M3 (QEMU mps2)";
}

uint32_t plat_cpu_hz(void)
{
    return ARM_SYSTICK_CLK;
}

const char *plat_board_name(void)
{
    return "QEMU mps2-an385";
}
