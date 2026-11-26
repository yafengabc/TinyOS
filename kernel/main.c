#include "types.h"
#include "platform.h"
#include "console.h"
#include "ramdisk.h"
#include "heap.h"
#include "mem_domain.h"
#include "process.h"
#include "cpu.h"
#include "devfs.h"
#include "periph.h"
#include "procfs.h"
#include "hwdev.h"

/* Architecture-independent entry point.  Everything platform specific goes
 * through platform.h; this file must build unchanged on i386, RV32 and ARM. */

static void banner(void)
{
    kprintf("\nTinyOS - a tiny multi-platform operating system\n");
    kprintf("user-mode ELF loader | littlefs | PIE\n");
    kprintf("kernel heap: %u KiB arena\n\n", heap_arena_bytes() / 1024u);
}

/* The panel, if the board has one, gets a title card of its own.  It is
 * written before the console mirror starts producing output, so the screen
 * opens with something that identifies the machine instead of a screenful
 * of boot text scrolling past. */
static void screen_banner(void)
{
    char line[48];

    if (screen_cols() <= 0)
        return;

    screen_puts("TinyOS  ");
    screen_puts(plat_board_name());
    screen_puts("\n");

    ksnprintf(line, sizeof(line), "tick %u Hz   heap %u KiB\n",
              (unsigned)plat_tick_hz(), heap_arena_bytes() / 1024u);
    screen_puts(line);
    screen_puts("uart0 115200   usb0 cdc\n");
    screen_puts("devices: cat /proc/dev\n");
    screen_puts("screen : /dev/lcd0\n");
    screen_puts("----------------------------------------\n");
}

/* kernel/board_check.c: generated board.conf numbers vs layout.c constants. */
void board_config_check(void);

/* Boot-progress hook: ports with a surviving boot log record which stage the
 * machine reached; the default does nothing.  See kernel/platform.h. */
__attribute__((weak)) void plat_boot_stage(uint32_t n) { (void)n; }

/* Flash unique id for the USB serial; ports without a readable id say no. */
__attribute__((weak)) int plat_flash_unique_id(uint8_t out[8])
{ (void)out; return 0; }

__attribute__((weak)) void plat_start_tasks(void)
{
}

/* No answer means "no protection": see kernel/plat/tasks.h. */
__attribute__((weak)) int plat_has_memprot(void) { return 0; }

void kmain(void)
{
    plat_early_init();
    plat_init();
    mem_domain_init();

    /* The scheduler runs off this tick: it is what preempts a task, wakes
       sleepers and hands console input to whoever is reading.  1000 Hz so
       that sleep(1) means 1 ms and a sampler can keep a millisecond
       cadence - see the QUANTUM note in process.c for what that costs. */
    plat_timer_init(1000);
    plat_boot_stage(1u);

    heap_init();
    plat_boot_stage(2u);
    ramdisk_init();
    plat_boot_stage(3u);

    /* Device files exist before the first task does.  Registration is a boot
     * act and the table is read-only afterwards, which is what lets open()
     * walk it on two CPUs without a lock.  A board with no GPIO registers
     * nothing and /dev/gpio0 simply does not open - the same answer the C
     * API gives when gpio_count() is 0. */
    devfs_init();
    periph_init_devs();
    procfs_init();
    plat_boot_stage(4u);

    /* Board devices: the table becomes /dev nodes here, and display 0
     * becomes a text screen the console can mirror onto.  A board with no
     * panel reports no display, both calls return -1, and every screen_*
     * call below is a no-op. */
    hwdev_init();
    screen_init();
    plat_boot_stage(5u);

    process_init();
    plat_boot_stage(6u);

    board_config_check();
    plat_boot_stage(7u);
    banner();
    plat_boot_stage(8u);
    screen_banner();

    /* Everything a secondary CPU touches exists from here on: the process
     * table, the console and the board's timer.  On a uniprocessor this
     * prints nothing and returns immediately; on the SMP build it wakes the
     * other harts and waits for them to report in, so that "2 of 2 online"
     * either appears before the shell does or the boot says why not. */
    cpu_init();
    plat_boot_stage(9u);

    /* Bring-up self-check: bounded wait for the first scheduler tick.  A
       dead timer otherwise boots fine here and fails confusingly later
       (preemption never happens, sleepers never wake); this turns it into
       a one-line diagnosis.  It is how the ESP32-C3 tick was finally
       verified on real hardware.
       MIE is normally deferred to the first idle, but the check needs it:
       turn it on here and leave it on - the runtime state after this point
       is exactly this, and the process tables are live already. */
    plat_irq_enable();
    plat_boot_stage(10u);
#if TOS_BOOT_TRACE
    {
        uint32_t spins = plat_pre_tick_spins(20000000u);
        while (plat_ticks() == 0 && --spins)
            ;
        kprintf("tick self-check: %u tick(s) after boot\n",
                (unsigned)plat_ticks());
    }
#endif

#ifdef TOS_WIFI
    /* TEMP WiFi probe: shell first, then the driver task. */
    {
        extern void wifi_test_start(void);
        wifi_test_start();
    }
#endif

#if TOS_BOOT_TRACE
    kprintf("\nstarting /bin/msh ...\n\n");
#endif
    plat_boot_stage(11u);
    /* Boards start their own kernel threads here: the scheduler and every
     * CPU are live, and the first user task has not been picked yet, so a
     * device thread (the RP2350 RV32 USB pump) is already running when the
     * shell starts. */
    plat_start_tasks();
#ifdef TOS_KTASK_TEST
    process_ktask_selftest();
#endif
    process_start_shell("/bin/msh");
    panic("shell returned");
}
