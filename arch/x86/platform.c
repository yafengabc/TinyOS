#include "platform.h"
#include "io.h"
#include "serial.h"
#include "timer.h"
#include "ata.h"
#include "idt.h"
#include "gdt.h"
#include "keyboard.h"
#include "console.h"
#include "string.h"

/* x86 (i386) implementation of the platform layer. */

/* ---- VGA text output ------------------------------------------------- */

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static uint16_t *const vga = (uint16_t *)0xB8000;
static int cur_x, cur_y;
static uint8_t color = 0x07;

static void vga_scroll(void)
{
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        vga[i] = vga[i + VGA_WIDTH];
    for (int i = 0; i < VGA_WIDTH; i++)
        vga[(VGA_HEIGHT - 1) * VGA_WIDTH + i] =
            (uint16_t)(' ' | ((uint16_t)color << 8));
    cur_y = VGA_HEIGHT - 1;
}

/* Writing to 0xB8000 moves the text but not the cursor: the blinking block
 * is drawn by the CRTC, which knows nothing about our frame buffer writes.
 * Without this it stays wherever the BIOS left it for the whole session. */
static void vga_cursor(void)
{
    uint16_t pos = (uint16_t)(cur_y * VGA_WIDTH + cur_x);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(pos >> 8));
}

static void vga_putc(char c)
{
    if (c == '\n') {
        cur_x = 0;
        if (++cur_y >= VGA_HEIGHT)
            vga_scroll();
    } else if (c == '\r') {
        cur_x = 0;
    } else if (c == '\b') {
        if (cur_x > 0)
            cur_x--;
        vga[cur_y * VGA_WIDTH + cur_x] = (uint16_t)(' ' | ((uint16_t)color << 8));
    } else {
        vga[cur_y * VGA_WIDTH + cur_x] =
            (uint16_t)((uint8_t)c | ((uint16_t)color << 8));
        if (++cur_x >= VGA_WIDTH) {
            cur_x = 0;
            if (++cur_y >= VGA_HEIGHT)
                vga_scroll();
        }
    }

    vga_cursor();
}

/* ---- boot ------------------------------------------------------------ */

void plat_early_init(void)
{
    /* The boot sector already established a stack at 0x9F000.  Enable the
     * serial port first so panics during the rest of init are visible. */
    serial_init();
    ata_init();
}

void plat_init(void)
{
    plat_console_init();
    gdt_init();
    idt_init();
    pic_remap();
    pic_mask_all_except(0x03);      /* IRQ0 timer, IRQ1 keyboard */

    /* No paging: the flat model needs no page tables and no frame
     * allocator.  RAM is used directly, exactly like on the MCUs.  The
     * periodic tick is programmed by kmain through plat_timer_init(). */
}

/* ---- console --------------------------------------------------------- */

void plat_console_init(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = (uint16_t)(' ' | ((uint16_t)color << 8));
    cur_x = cur_y = 0;
    vga_cursor();
}

void plat_console_putc(char c)
{
    vga_putc(c);
    serial_putc(c);
}

void plat_console_flush(void)
{
    serial_flush();
}

/* serial_putc() already drains the receive FIFO while it waits for THRE
 * (see serial_putc_raw), but reporting room keeps echo out of that wait
 * altogether and costs one port read. */
int plat_console_tx_room(void)
{
    return (inb(COM1 + 5) & 0x20) ? 16 : 0;   /* LSR: THRE */
}

int plat_console_getchar(void)
{
    /* Input comes from the PS/2 keyboard or the serial console. */
    serial_poll();
    int c = keyboard_getchar();
    if (c >= 0)
        return c;
    if (serial_has_input())
        return serial_read();
    return -1;
}

/* ---- timer / interrupts ---------------------------------------------- */

void     plat_timer_init(uint32_t hz) { timer_init(hz); }
uint64_t plat_ticks(void)             { return timer_ticks(); }
uint32_t plat_tick_hz(void)           { return timer_hz(); }

void plat_irq_enable(void)  { sti(); }
void plat_irq_disable(void) { cli(); }

/* EFLAGS.IF, captured and cleared in one sequence. */
uint32_t plat_irq_save(void)
{
    uint32_t flags;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags & 0x200u;
}

void plat_irq_restore(uint32_t state)
{
    if (state)
        __asm__ volatile("sti" ::: "memory");
}
void plat_cpu_idle(void)    { hlt(); }

uint32_t plat_uptime_ms(void)      { return timer_uptime_ms(); }

/* The PC port has no free-running counter wired up (the PIT only ticks at
 * 100 Hz and reading it back would need channel 2 to be sacrificed), so the
 * clock is the tick: monotonic, 10 ms of resolution.  Good enough for
 * `uptime` and for timing anything that is not microscopic. */
uint64_t plat_now_ns(void)         { return (uint64_t)timer_ticks() * 10000000ull; }

/* One CPU, always. */
int plat_cpu_id(void)              { return 0; }

/* ---- block device ---------------------------------------------------- */

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!ata_present())
        return -1;
    return ata_read_sectors(ata_io_base(), ata_drive_no(), lba,
                            (uint16_t)count, buf);
}

int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!ata_present())
        return -1;
    return ata_write_sectors(ata_io_base(), ata_drive_no(), lba,
                             (uint16_t)count, buf);
}

int plat_block_erase(uint32_t lba, uint32_t count)
{
    /* ATA has no erase concept, but littlefs wants its blocks to read
     * 0xFF before first prog - emulate with a fill through the normal
     * write path. */
    uint8_t ff[512];
    uint32_t i;
    memset(ff, 0xFF, sizeof(ff));
    for (i = 0; i < count; i++)
        if (plat_block_write(lba + i, 1, ff) != 0)
            return -1;
    return 0;
}

uint32_t plat_block_size(void)  { return 512; }
uint32_t plat_block_count(void) { return 0; }

/* ---- user mode ------------------------------------------------------- */

void plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp)
{
    memset(frame, 0, sizeof(*frame));
    frame->gs = USER_DS;
    frame->fs = USER_DS;
    frame->es = USER_DS;
    frame->ds = USER_DS;
    frame->eip = entry;
    frame->cs = USER_CS;
    frame->eflags = 0x202;
    frame->useresp = sp;
    frame->ss = USER_DS;
}

/* i386 syscall convention: eax = number, ebx/ecx/edx = arguments,
 * eax = return value. */
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
    /* Let the last line reach the serial port before the power goes off:
     * halting first would drop the tail of the message. */
    console_shutdown();

    /* ACPI power off (works in QEMU and most modern machines) */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    cli();
    for (;;)
        hlt();
}

void plat_set_kernel_stack(uint32_t top) { tss_set_kernel_stack(top); }

/* Only a ring 3 frame may be switched away from: a trap taken in ring 0
 * has kernel call frames under it that cannot be resumed later. */
int plat_fault_is_user(void *frame)
{
    return (((const regs_t *)frame)->cs & 3) == 3;
}

/* Ring 3 is the only isolation this port has: paging is off on purpose
 * (docs/PLATFORM-DESIGN.md), and without it segments cannot express a
 * per-task boundary.  So nothing to program - the rv32 port is where
 * plat_task_protect does real work. */
void plat_task_protect(uint32_t win_base, uint32_t win_end, uint32_t wx,
                       uint32_t brk, uint32_t stack_low, uint32_t stack_top)
{
    (void)win_base; (void)win_end; (void)wx; (void)brk;
    (void)stack_low; (void)stack_top;
}

/* The trap frame layout belongs to the arch, so core hands us an opaque
 * pointer and we cast it to the i386 register frame here. */
extern void enter_user_frame(void *frame) __attribute__((noreturn));

void plat_enter_user(void *frame)
{
    enter_user_frame(frame);
}

void plat_panic_halt(void)
{
    cli();
    for (;;)
        hlt();
}

void plat_reboot(void)
{
    /* Same reason as plat_shutdown: the 16550 still holds the tail of the
     * message and the reset does not flush it. */
    console_shutdown();
    outb(0x64, 0xFE);      /* pulse the keyboard controller reset line */
    cli();
    for (;;)
        hlt();
}

/* The PC target has no CPUID-derived brand string, and there is no
 * meaningful clock to report: QEMU/TCG runs the guest at whatever the host
 * allows. */
const char *plat_cpu_isa(void)
{
    return "i686";
}

const char *plat_cpu_model(void)
{
    return "i686 (PC)";
}

uint32_t plat_cpu_hz(void)
{
    return 0;
}

const char *plat_board_name(void)
{
    return "PC (i686)";
}
