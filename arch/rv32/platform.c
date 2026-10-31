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

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)(clint_time() / (RV32_TIMER_HZ / 1000u));
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
        process_tick(f);
        return process_resched(f);
    }

    if (cause == RV_MCAUSE_ECALL_U) {
        /* Step over the ecall first, so the frame reads like the i386 one
         * (where the CPU has already pushed the return address): exec and
         * exit save/replace this frame, and the saved copy must resume
         * after the syscall rather than re-execute it.  ecall is always a
         * 4-byte instruction. */
        f->mepc += 4;
        return syscall_dispatch(f);
    }

    if (from_user(f)) {
        /* A user program trapped: kill it and keep the system alive. */
        kprintf("\n[pid %d] %s at 0x%08x", process_current_pid(),
                cause_name(cause), f->mepc);
        if (cause == RV_MCAUSE_LOAD_FAULT || cause == RV_MCAUSE_STORE_FAULT)
            kprintf(" (address 0x%08x)", f->mtval);
        kprintf("\n");
        return process_exit(f, 139);
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

/* U-mode reaches memory only through PMP.  Entries 4..15 stay off, so
 * until the first task is programmed the user side can touch nothing at
 * all - M-mode is not subject to PMP as long as no entry sets the lock
 * bit, which is what keeps the flat kernel model working.
 *
 * All four config registers exist on qemu-virt and on the ESP32-C3 (16
 * entries).  A core with fewer would need the count probed first, and a
 * bootloader that locked its entries cannot be overruled at all - both are
 * problems for the board port, not for this one. */
static void pmp_deny_user(void)
{
    __asm__ volatile("csrw pmpcfg0, zero");
    __asm__ volatile("csrw pmpcfg1, zero");
    __asm__ volatile("csrw pmpcfg2, zero");
    __asm__ volatile("csrw pmpcfg3, zero");
}

void plat_early_init(void)
{
    plat_console_init();
}

void plat_init(void)
{
    plat_console_init();
    pmp_deny_user();
}

/* ---- user mode ------------------------------------------------------- */

/* Per-task PMP.  TOR is positional - entry i covers [pmpaddr[i-1]<<2,
 * pmpaddr[i]<<2) - and the lower bound comes from the previous address
 * register even when that entry is disabled.  Since entry 0 can only
 * start at address 0, that is how a region below the window is denied:
 * entry 0 is off and holds nothing but the lower bound.
 *
 *   0  off    win_base                     lower bound only
 *   1  RX     [win_base, wx)               code
 *   2  RW     [wx, brk)                    data and heap
 *   3  RW     [stack_low, stack_top)       stack, NAPOT
 *
 * Nothing claims the hole between brk and stack_low, so a heap that runs
 * off its end and a stack that runs off its bottom both fault instead of
 * quietly eating the other.  The stack uses NAPOT because it is absolute:
 * a TOR entry there would have to start at brk and would fill that hole. */
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u
#define PMP_TOR   0x08u
#define PMP_NAPOT 0x18u

void plat_task_protect(uint32_t win_base, uint32_t wx, uint32_t brk,
                       uint32_t stack_low, uint32_t stack_top)
{
    uint32_t size = stack_top - stack_low;

    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(win_base >> 2));
    __asm__ volatile("csrw pmpaddr1, %0" : : "r"((wx  & ~3u) >> 2));
    __asm__ volatile("csrw pmpaddr2, %0" : : "r"((brk & ~3u) >> 2));
    /* NAPOT: all ones above the size bit, so 32 KiB is (32K>>3)-1 = 0xFFF. */
    __asm__ volatile("csrw pmpaddr3, %0" : :
                     "r"((stack_low >> 2) | ((size >> 3) - 1u)));

    uint32_t cfg = ((PMP_TOR   | PMP_R | PMP_X) << 8)    /* entry 1 */
                 | ((PMP_TOR   | PMP_R | PMP_W) << 16)   /* entry 2 */
                 | ((PMP_NAPOT | PMP_R | PMP_W) << 24);  /* entry 3 */
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(cfg));
}

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
