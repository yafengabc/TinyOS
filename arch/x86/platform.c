#include "platform.h"
#include "io.h"
#include "paging.h"
#include "pmm.h"
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

static void vga_putc(char c)
{
    if (c == '\n') {
        cur_x = 0;
        if (++cur_y >= VGA_HEIGHT)
            vga_scroll();
        return;
    }
    if (c == '\r') {
        cur_x = 0;
        return;
    }
    if (c == '\b') {
        if (cur_x > 0)
            cur_x--;
        vga[cur_y * VGA_WIDTH + cur_x] = (uint16_t)(' ' | ((uint16_t)color << 8));
        return;
    }
    vga[cur_y * VGA_WIDTH + cur_x] =
        (uint16_t)((uint8_t)c | ((uint16_t)color << 8));
    if (++cur_x >= VGA_WIDTH) {
        cur_x = 0;
        if (++cur_y >= VGA_HEIGHT)
            vga_scroll();
    }
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

    plat_mem_init();
    paging_init();
    timer_init();
}

/* ---- console --------------------------------------------------------- */

void plat_console_init(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = (uint16_t)(' ' | ((uint16_t)color << 8));
    cur_x = cur_y = 0;
}

void plat_console_putc(char c)
{
    vga_putc(c);
    serial_putc(c);
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

void     plat_timer_init(uint32_t hz) { (void)hz; timer_init(); }
uint64_t plat_ticks(void)             { return timer_ticks(); }
uint32_t plat_tick_hz(void)           { return 100; }

void plat_irq_enable(void)  { sti(); }
void plat_irq_disable(void) { cli(); }
void plat_cpu_idle(void)    { hlt(); }

void plat_cpu_idle_poll(void)
{
    /* Keep draining the UART while waiting; the timer tick also polls. */
    __asm__ volatile("sti; pause");
}

uint32_t plat_uptime_ms(void)      { return timer_uptime_ms(); }
void     plat_sleep_ms(uint32_t ms) { timer_sleep_ms(ms); }

int plat_user_ok(uint32_t as, uint32_t addr, uint32_t len)
{
    return paging_user_ok(as, addr, len);
}

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

int plat_block_erase(uint32_t lba)
{
    (void)lba;
    return 0;
}

uint32_t plat_block_size(void)  { return 512; }
uint32_t plat_block_count(void) { return 0; }

/* ---- address translation (for the ELF loader) ------------------------ */

uint32_t plat_translate(uint32_t as, uint32_t va)
{
    return paging_translate(as, va);
}

int plat_map_range(uint32_t as, uint32_t va, uint32_t len)
{
    if (len == 0)
        return 0;
    uint32_t start = va & ~0xFFFu;
    uint32_t end = (va + len + 0xFFFu) & ~0xFFFu;

    for (uint32_t v = start; v < end; v += PAGE_SIZE) {
        if (paging_translate(as, v))
            continue;
        uint32_t frame = pmm_alloc();
        if (!frame)
            return -1;
        if (paging_map(as, v, frame, PTE_PRESENT | PTE_WRITE | PTE_USER) != 0)
            return -1;
    }
    return 0;
}

/* ---- physical memory ------------------------------------------------- */

void plat_mem_init(void)  { pmm_init(); }uint32_t plat_alloc_page(void) { return pmm_alloc(); }
void plat_free_page(uint32_t p) { pmm_free(p); }

/* ---- user mode ------------------------------------------------------- */

uint32_t plat_as_create(void)
{
    return paging_create_as();
}

void plat_as_destroy(uint32_t as)
{
    paging_destroy_as(as);
}

uint32_t plat_current_as(void)
{
    return read_cr3();
}

int plat_user_page(uint32_t as, uint32_t va)
{
    if (paging_translate(as, va))
        return 0;
    uint32_t frame = pmm_alloc();
    if (!frame)
        return -1;
    return paging_map(as, va, frame, PTE_PRESENT | PTE_WRITE | PTE_USER);
}

void plat_activate_as(uint32_t as)
{
    write_cr3(as);
}

void plat_flush_tlb(uint32_t va)
{
    invlpg(va);
}

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

void plat_write_user(uint32_t as, uint32_t va, const void *src, uint32_t len)
{
    paging_write(as, va, src, len);
}

void plat_shutdown(void)
{
    /* ACPI power off (works in QEMU and most modern machines) */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    cli();
    for (;;)
        hlt();
}

void plat_set_kernel_stack(uint32_t top) { tss_set_kernel_stack(top); }
int  plat_fault_is_user(void *frame)     { (void)frame; return 1; }

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
    outb(0x64, 0xFE);      /* pulse the keyboard controller reset line */
    cli();
    for (;;)
        hlt();
}
