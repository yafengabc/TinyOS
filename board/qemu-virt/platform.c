#include "platform.h"
#include "board.h"
#include "rv32_memmap.h"
#include "console.h"
#include "string.h"

/* Board: QEMU virt (rv32).
 *
 * 16550 UART console, CLINT timer, a RAM disk backing the littlefs
 * partition, and the SiFive test device for shutdown/reset. */

/* ---- 16550 UART (QEMU virt maps UART0 here) -------------------------- */

#define UART_REG_DATA 0x00
#define UART_REG_IER  0x01
#define UART_REG_FCR  0x02
#define UART_REG_LCR  0x03
#define UART_REG_LSR  0x05
#define UART_LSR_DR   0x01
#define UART_LSR_THRE 0x20
#define UART_LSR_TEMT 0x40      /* THR and shift register both empty */

void plat_console_init(void)
{
    mmio_write8(RV32_UART_BASE + UART_REG_IER, 0x00);   /* no UART irqs */
    mmio_write8(RV32_UART_BASE + UART_REG_LCR, 0x03);   /* 8N1          */
    mmio_write8(RV32_UART_BASE + UART_REG_FCR, 0x07);   /* reset FIFOs  */
}

void plat_console_putc(char c)
{
    while (!(mmio_read8(RV32_UART_BASE + UART_REG_LSR) & UART_LSR_THRE))
        ;
    mmio_write8(RV32_UART_BASE + UART_REG_DATA, (uint8_t)c);
}

/* console_shutdown() drains the transmitter before a shutdown or reset. */
void plat_console_flush(void)
{
    while (!(mmio_read8(RV32_UART_BASE + UART_REG_LSR) & UART_LSR_TEMT))
        ;
}

int plat_console_getchar(void)
{
    if (!(mmio_read8(RV32_UART_BASE + UART_REG_LSR) & UART_LSR_DR))
        return -1;
    return mmio_read8(RV32_UART_BASE + UART_REG_DATA);
}

/* The 16550 holds 16 bytes; report what is free instead of letting echo block
 * the tick (see the comment on console_echo). */
int plat_console_tx_room(void)
{
    return (mmio_read8(RV32_UART_BASE + UART_REG_LSR) & UART_LSR_THRE) ? 16 : 0;
}

/* ---- CLINT timer ----------------------------------------------------- */

static volatile uint64_t g_ticks;
static uint32_t g_interval;

static uint64_t clint_time(void)
{
    /* mtime is 64 bit; read low then high, retry if low wrapped */
    uint32_t hi1, lo, hi2;
    do {
        hi1 = mmio_read32(RV32_CLINT_BASE + RV32_CLINT_MTIMEH);
        lo  = mmio_read32(RV32_CLINT_BASE + RV32_CLINT_MTIME);
        hi2 = mmio_read32(RV32_CLINT_BASE + RV32_CLINT_MTIMEH);
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | lo;
}

/* mtimecmp is one register per hart, eight bytes apart: hart 0 at +0x4000,
 * hart 1 at +0x4008.  Writing hart 0's compare from hart 1 would arm the
 * wrong CPU and leave the caller's own interrupt tied off for ever - the
 * single-CPU version of this file could get away with the fixed address
 * because there was only ever one hart. */
static void clint_set_cmp(int hart, uint64_t v)
{
    uint32_t base = RV32_CLINT_BASE + RV32_CLINT_MTIMECMP + 8u * (uint32_t)hart;

    mmio_write32(base + 4, (uint32_t)0xFFFFFFFFu);
    mmio_write32(base + 0, (uint32_t)v);
    mmio_write32(base + 4, (uint32_t)(v >> 32));
}

/* MTIE = bit 7 of mie, which is a per-hart CSR as well. */
static void timer_unmask(void)
{
    uint32_t mie;

    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    mie |= (1u << 7);
    __asm__ volatile("csrw mie, %0" : : "r"(mie));
}

void plat_timer_init(uint32_t hz)
{
    /* The virt machine runs mtime at 10 MHz. */
    g_interval = RV32_TIMER_HZ / hz;
    g_ticks = 0;
    clint_set_cmp(plat_cpu_id(), clint_time() + g_interval);
    timer_unmask();
}

void plat_timer_start_cpu(void)
{
    if (plat_cpu_id() == 0)
        return;                 /* already armed by plat_timer_init */

    clint_set_cmp(plat_cpu_id(), clint_time() + g_interval);
    timer_unmask();
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return RV32_TIMER_HZ / g_interval;
}

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)(clint_time() / (RV32_TIMER_HZ / 1000u));
}

/* The real clock: mtime is a free-running 64-bit counter, so the tick is
 * only a scheduling detail and time resolution is 100 ns. */
uint64_t plat_now_ns(void)
{
    return clint_time() * (1000000000ull / RV32_TIMER_HZ);
}

void plat_timer_tick(void)
{
    int id = plat_cpu_id();

    /* Only the boot CPU counts time.  Every hart needs its own tick (it is
     * what preempts whatever that hart is running), but the counter they
     * share is a global one: incrementing it on both would make the clock
     * run at 2x and every sleep deadline expire twice as fast.  The boot
     * CPU's timer keeps running even when it is idle, so this stays a true
     * 100 Hz wall clock. */
    if (id == 0)
        g_ticks++;
    clint_set_cmp(id, clint_time() + g_interval);
}

/* The CLINT machine timer is the only interrupt this board routes, and the
 * arch dispatcher reports it as cause 7 (RV_MCAUSE_M_TIMER). */
int plat_irq_is_tick(uint32_t line)
{
    return line == 7u;
}

void plat_irq_service(uint32_t line)
{
    (void)line;          /* nothing else is routed on this board */
}

/* ---- block device: RAM disk ------------------------------------------ */

/* The build tool appends the littlefs image to the kernel ELF (see
 * internal/build), so the partition is simply a writable buffer in RAM.
 * PLAT_LFS_LBA is 0 here: block 0 of the device is block 0 of the
 * filesystem. */
extern uint8_t __ramdisk_start[];
extern uint8_t __ramdisk_end[];

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    uint32_t off = lba * 512u;
    uint32_t len = count * 512u;
    if (lba != 0 && off < lba)                       /* overflow */
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
    /* Nothing beyond what plat_timer_init does. */
}

/* ---- power ------------------------------------------------------------ */

void plat_shutdown(void)
{
    /* QEMU virt's SiFive test device: 0x5555 powers the machine off. */
    mmio_write32(RV32_TEST_BASE, 0x5555u);
    plat_panic_halt();
}

void plat_reboot(void)
{
    /* 0x7777 is the reset request of the same test device.  Drain the UART
     * first - it is emulated with a real output queue. */
    console_shutdown();
    mmio_write32(RV32_TEST_BASE, 0x7777u);
    plat_panic_halt();
}

/* QEMU does not expose a meaningful core clock; the CLINT's 10 MHz is the
 * timer rate, not the CPU's.  /proc/cpuinfo prints "unknown" for it. */
const char *plat_cpu_model(void)
{
    return "QEMU virt RISC-V";
}

uint32_t plat_cpu_hz(void)
{
    return 0;
}

const char *plat_board_name(void)
{
    return "QEMU virt (RV32)";
}
