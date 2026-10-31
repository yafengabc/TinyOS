#include "platform.h"
#include "rv32_memmap.h"
#include "console.h"
#include "string.h"
#include "process.h"

/* RV32 platform layer for QEMU virt (and later ESP32-C3).
 *
 * 16550 console, CLINT timer, machine-mode traps, U-mode via PMP and a
 * RAM disk that holds the littlefs partition.  There is no MMU on this
 * target: all addresses are physical and task windows come from the same
 * bump allocator the x86 port uses (kernel/process.c). */

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
    __asm__ volatile("csrsi mstatus, 8");   /* mstatus.MIE = bit 3 */
}

void plat_irq_disable(void)
{
    __asm__ volatile("csrci mstatus, 8");
}

void plat_cpu_idle(void)
{
    __asm__ volatile("csrsi mstatus, 8; wfi");
}

void plat_cpu_idle_poll(void)
{
    /* Re-enable interrupts while polling so the tick keeps advancing; the
       caller is already spinning on the UART. */
    __asm__ volatile("csrsi mstatus, 8; nop");
}

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)(clint_time() / (RV32_TIMER_HZ / 1000u));
}

void plat_sleep_ms(uint32_t ms)
{
    uint64_t target = clint_time() + (uint64_t)ms * (RV32_TIMER_HZ / 1000u);
    while (clint_time() < target)
        __asm__ volatile("csrsi mstatus, 8; wfi");
}

/* ---- traps ----------------------------------------------------------- */

static int from_user(const regs_t *f)
{
    return (f->mstatus & RV_MSTATUS_MPP_MASK) == RV_MSTATUS_MPP_U;
}

static const char *cause_name(uint32_t cause)
{
    switch (cause) {
    case 0:  return "instruction address misaligned";
    case 1:  return "instruction access fault";
    case RV_MCAUSE_ILLEGAL:      return "illegal instruction";
    case 3:  return "breakpoint";
    case 4:  return "load address misaligned";
    case RV_MCAUSE_LOAD_FAULT:   return "load access fault";
    case 6:  return "store address misaligned";
    case RV_MCAUSE_STORE_FAULT:  return "store access fault";
    case RV_MCAUSE_ECALL_U:      return "ecall from U-mode";
    case 11: return "ecall from M-mode";
    default: return "unknown trap";
    }
}

regs_t *trap_handler(regs_t *f)
{
    uint32_t cause = f->mcause;

    if (cause == RV_MCAUSE_IRQ_M_TIMER) {
        g_ticks++;
        clint_set_cmp(clint_time() + g_interval);
        return f;
    }

    if (cause == RV_MCAUSE_ECALL_U) {
        /* Step over the ecall first, so the frame reads like the i386 one
         * (where the CPU has already pushed the return address): exec and
         * exit save/replace this frame, and the saved copy must resume
         * after the syscall rather than re-execute it.  ecall is always a
         * 4-byte instruction. */
        f->mepc += 4;
        syscall_dispatch(f);
        return f;
    }

    if (from_user(f)) {
        /* A user program trapped: kill it and keep the system alive. */
        kprintf("\n[pid %d] %s at 0x%08x", process_current_pid(),
                cause_name(cause), f->mepc);
        if (cause == RV_MCAUSE_LOAD_FAULT || cause == RV_MCAUSE_STORE_FAULT)
            kprintf(" (address 0x%08x)", f->mtval);
        kprintf("\n");
        process_kill_current(f, 139);
        return f;
    }

    kprintf("\n*** RV32 TRAP ***\n");
    kprintf("%s: mcause=0x%x mepc=0x%x mtval=0x%x\n",
            cause_name(cause), cause, f->mepc, f->mtval);
    plat_panic_halt();
    return f;
}

/* ---- physical memory ------------------------------------------------- */

/* The flat model needs no frame allocator: RAM is directly addressable.
 * Task windows are carved out by kernel/process.c. */

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

int plat_block_erase(uint32_t lba)
{
    uint32_t off = lba * 512u;
    if (off + 512u > (uint32_t)(__ramdisk_end - __ramdisk_start))
        return -1;
    memset(__ramdisk_start + off, 0xFF, 512);
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

static void pmp_open_all(void)
{
    /* U-mode checks every access against PMP.  Without an entry covering
       the address space, the first user instruction faults.  One NAPOT
       region of 8 GiB starting at 0 grants RWX everywhere - the flat
       model deliberately has no memory protection, so this matches the
       capability of the MCUs we target (see docs/PLATFORM-DESIGN.md). */
    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(0x3FFFFFFFu));
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(0x1Fu));   /* A=NAPOT, RWX */
}

void plat_early_init(void)
{
    plat_console_init();
}

void plat_init(void)
{
    plat_console_init();
    pmp_open_all();
}

/* ---- user mode ------------------------------------------------------- */

void plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp)
{
    memset(frame, 0, sizeof(*frame));
    frame->sp = sp;
    frame->mepc = entry;
    /* Return to U-mode with interrupts "enabled" (MPIE), so mret leaves
       the machine in the same state a fresh trap from user code finds. */
    frame->mstatus = RV_MSTATUS_MPP_U | RV_MSTATUS_MPIE;
}

/* RV32 syscall convention: a7 = number, a0/a1/a2 = arguments, a0 = result. */
void plat_frame_get_syscall(const regs_t *r, syscall_t *sc)
{
    sc->nr = r->a7;
    sc->a0 = r->a0;
    sc->a1 = r->a1;
    sc->a2 = r->a2;
}

void plat_frame_set_ret(regs_t *r, uint32_t val) { r->a0 = val; }
uint32_t plat_frame_get_ret(const regs_t *r)    { return r->a0; }

int plat_fault_is_user(void *frame)
{
    return from_user((const regs_t *)frame);
}

/* Implemented in start.S: restores the frame and mret's into it. */
extern void rv32_enter_user(void *frame) __attribute__((noreturn));

void plat_enter_user(void *frame)
{
    rv32_enter_user(frame);
}

void plat_set_kernel_stack(uint32_t top)
{
    /* The kernel stack lives in .bss and start.S computes it directly;
       there is no TSS to update. */
    (void)top;
}

void plat_shutdown(void)
{
    /* QEMU virt's SiFive test device: 0x5555 powers the machine off. */
    mmio_write32(RV32_TEST_BASE, 0x5555u);
    plat_panic_halt();
}

void plat_panic_halt(void)
{
    __asm__ volatile("csrci mstatus, 8");
    for (;;)
        __asm__ volatile("wfi");
}

void plat_reboot(void)
{
    /* 0x7777 is the reset request of the same test device. */
    mmio_write32(RV32_TEST_BASE, 0x7777u);
    plat_panic_halt();
}
