#include "platform.h"
#include "board.h"
#include "rv32_memmap.h"
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

int plat_console_getchar(void)
{
    if (!(mmio_read8(RV32_UART_BASE + UART_REG_LSR) & UART_LSR_DR))
        return -1;
    return mmio_read8(RV32_UART_BASE + UART_REG_DATA);
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

static void clint_set_cmp(uint64_t v)
{
    mmio_write32(RV32_CLINT_BASE + RV32_CLINT_MTIMECMPH, (uint32_t)0xFFFFFFFFu);
    mmio_write32(RV32_CLINT_BASE + RV32_CLINT_MTIMECMP, (uint32_t)v);
    mmio_write32(RV32_CLINT_BASE + RV32_CLINT_MTIMECMPH, (uint32_t)(v >> 32));
}

void plat_timer_init(uint32_t hz)
{
    /* The virt machine runs mtime at 10 MHz. */
    g_interval = RV32_TIMER_HZ / hz;
    g_ticks = 0;
    clint_set_cmp(clint_time() + g_interval);

    /* Enable machine timer interrupt (MTIE = bit 7). */
    uint32_t mie;
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    mie |= (1u << 7);
    __asm__ volatile("csrw mie, %0" : : "r"(mie));
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

void plat_timer_tick(void)
{
    g_ticks++;
    clint_set_cmp(clint_time() + g_interval);
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
    /* 0x7777 is the reset request of the same test device. */
    mmio_write32(RV32_TEST_BASE, 0x7777u);
    plat_panic_halt();
}
