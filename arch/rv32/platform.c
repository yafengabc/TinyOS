#include "platform.h"
#include "rv32_memmap.h"
#include "console.h"
#include "string.h"

/* RV32 platform layer for QEMU virt (and later ESP32-C3).
 *
 * P1 scope: 16550 console, CLINT timer, machine-mode traps.  There is no
 * MMU on this target: all addresses are physical and task windows come from
 * the same bump allocator the x86 port uses (kernel/process.c). */

/* ---- 16550 UART (QEMU virt maps UART0 here) -------------------------- */

#define UART_REG_DATA 0x00
#define UART_REG_IER  0x01
#define UART_REG_FCR  0x02
#define UART_REG_LCR  0x03
#define UART_REG_LSR  0x05
#define UART_LSR_DR   0x01
#define UART_LSR_THRE 0x20

static inline void mmio_write8(uint32_t addr, uint8_t v)
{
    *(volatile uint8_t *)addr = v;
}

static inline uint8_t mmio_read8(uint32_t addr)
{
    return *(volatile uint8_t *)addr;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

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

void plat_irq_enable(void)
{
    /* mstatus.MIE = bit 3 */
    __asm__ volatile("csrsi mstatus, 8");
}

void plat_irq_disable(void)
{
    __asm__ volatile("csrci mstatus, 8");
}

void plat_cpu_idle(void)
{
    __asm__ volatile("wfi");
}

void plat_cpu_idle_poll(void)
{
    __asm__ volatile("nop");
}

uint32_t plat_uptime_ms(void)
{
    /* mtime runs at RV32_TIMER_HZ. */
    return (uint32_t)(clint_time() / (RV32_TIMER_HZ / 1000u));
}

void plat_sleep_ms(uint32_t ms)
{
    uint64_t target = clint_time() + (uint64_t)ms * (RV32_TIMER_HZ / 1000u);
    while (clint_time() < target)
        __asm__ volatile("wfi");
}

/* ---- traps ----------------------------------------------------------- */

#include "trap.h"

uint32_t trap_handler(rv32_frame_t *f)
{
    uint32_t cause = f->mcause;

    if (cause == RV_MCAUSE_IRQ_M_TIMER) {
        g_ticks++;
        clint_set_cmp(clint_time() + g_interval);
        return (uint32_t)f;
    }

    /* Anything else is fatal for now: report and halt. */
    kprintf("\n*** RV32 TRAP ***\n");
    kprintf("mcause=0x%x mepc=0x%x mtval=0x%x\n", cause, f->mepc, f->mtval);
    plat_panic_halt();
    return (uint32_t)f;
}

/* ---- physical memory ------------------------------------------------- */

/* The flat model needs no frame allocator: RAM is directly addressable.
 * Task windows are carved out by kernel/process.c. */

/* ---- block device (RAM disk for now) --------------------------------- */

static uint32_t g_ramdisk_page;

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    (void)lba;
    (void)count;
    (void)buf;
    return -1;      /* not implemented in P1 */
}

int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}

int plat_block_erase(uint32_t lba)
{
    (void)lba;
    return -1;
}

uint32_t plat_block_size(void)
{
    return 512;
}

uint32_t plat_block_count(void)
{
    (void)g_ramdisk_page;
    return 0;
}

/* ---- boot ------------------------------------------------------------ */

void plat_early_init(void)
{
    plat_console_init();
}

void plat_init(void)
{
    plat_console_init();
}

/* ---- user mode (P2) -------------------------------------------------- */

void plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp)
{
    memset(frame, 0, sizeof(*frame));
    frame->eip = entry;
    frame->useresp = sp;
}

/* RV32 syscall convention: a7 = number, a0/a1/a2 = arguments.
 * TODO(P2): rv32 has its own trap frame (arch/rv32/trap.h); map those
 * registers here once ecall/U-mode lands. */
void plat_frame_get_syscall(const regs_t *r, syscall_t *sc)
{
    sc->nr = r->eax;
    sc->a0 = r->ebx;
    sc->a1 = r->ecx;
    sc->a2 = r->edx;
}

void plat_frame_set_ret(regs_t *r, uint32_t val) { r->eax = val; }
uint32_t plat_frame_get_ret(const regs_t *r)    { return r->eax; }

void plat_shutdown(void)
{
    /* QEMU virt: the SBI "shutdown" call would be used on real firmware;
     * for now just halt. */
    for (;;)
        __asm__ volatile("wfi");
}

void plat_set_kernel_stack(uint32_t top)
{
    (void)top;
}

void plat_enter_user(void *frame)
{
    (void)frame;
    plat_panic_halt();
}

int plat_fault_is_user(void *frame)
{
    (void)frame;
    return 0;
}

void plat_panic_halt(void)
{
    __asm__ volatile("csrci mstatus, 8");
    for (;;)
        __asm__ volatile("wfi");
}

void plat_reboot(void)
{
    plat_panic_halt();
}
