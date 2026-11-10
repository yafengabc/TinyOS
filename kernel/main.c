#include "types.h"
#include "platform.h"
#include "console.h"
#include "ramdisk.h"
#include "heap.h"
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

void kmain(void)
{
    plat_early_init();
    plat_init();

    /* The scheduler runs off this tick: it is what preempts a task, wakes
       sleepers and hands console input to whoever is reading.  1000 Hz so
       that sleep(1) means 1 ms and a sampler can keep a millisecond
       cadence - see the QUANTUM note in process.c for what that costs. */
    plat_timer_init(1000);

    heap_init();
    ramdisk_init();

    /* Device files exist before the first task does.  Registration is a boot
     * act and the table is read-only afterwards, which is what lets open()
     * walk it on two CPUs without a lock.  A board with no GPIO registers
     * nothing and /dev/gpio0 simply does not open - the same answer the C
     * API gives when gpio_count() is 0. */
    devfs_init();
    periph_init_devs();
    procfs_init();

    /* Board devices: the table becomes /dev nodes here, and display 0
     * becomes a text screen the console can mirror onto.  A board with no
     * panel reports no display, both calls return -1, and every screen_*
     * call below is a no-op. */
    hwdev_init();
    screen_init();

    process_init();

    banner();
    screen_banner();

    /* Everything a secondary CPU touches exists from here on: the process
     * table, the console and the board's timer.  On a uniprocessor this
     * prints nothing and returns immediately; on the SMP build it wakes the
     * other harts and waits for them to report in, so that "2 of 2 online"
     * either appears before the shell does or the boot says why not. */
    cpu_init();

    /* Bring-up self-check: bounded wait for the first scheduler tick.  A
       dead timer otherwise boots fine here and fails confusingly later
       (preemption never happens, sleepers never wake); this turns it into
       a one-line diagnosis.  It is how the ESP32-C3 tick was finally
       verified on real hardware.
       MIE is normally deferred to the first idle, but the check needs it:
       turn it on here and leave it on - the runtime state after this point
       is exactly this, and the process tables are live already. */
    plat_irq_enable();
    {
        uint32_t spins = 20000000;
        while (plat_ticks() == 0 && --spins)
            ;
        kprintf("tick self-check: %u tick(s) after boot\n",
                (unsigned)plat_ticks());
    }

#ifdef TOS_WIFI
    /* TEMP WiFi probe: shell first, then the driver task. */
    {
        extern void wifi_test_start(void);
        wifi_test_start();
    }
#endif

    kprintf("\nstarting /bin/msh ...\n\n");
    process_start_shell("bin/msh");
    panic("shell returned");
}
