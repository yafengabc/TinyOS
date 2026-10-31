#include "types.h"
#include "platform.h"
#include "console.h"
#include "ramdisk.h"
#include "heap.h"
#include "process.h"

/* Architecture-independent entry point.  Everything platform specific goes
 * through platform.h; this file must build unchanged on i386, RV32 and ARM. */

static void banner(void)
{
    kprintf("\nTinyOS - a tiny multi-platform operating system\n");
    kprintf("user-mode ELF loader | littlefs | PIE\n");
    kprintf("kernel heap: %u KiB arena\n\n", heap_arena_bytes() / 1024u);
}

static void list_files(void)
{
    kprintf("littlefs: %u files\n", ramdisk_count());
    for (uint32_t i = 0; i < ramdisk_count(); i++) {
        uint32_t size = 0;
        const char *name = ramdisk_name(i, &size);
        kprintf("  %s (%u bytes)\n", name, size);
    }
}

void kmain(void)
{
    plat_early_init();
    plat_init();

    /* The scheduler runs off this tick: it is what preempts a task, wakes
       sleepers and hands console input to whoever is reading. */
    plat_timer_init(100);

    heap_init();
    ramdisk_init();
    process_init();

    banner();
    list_files();

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

    kprintf("\nstarting /bin/sh ...\n\n");
    process_start_shell("bin/sh");
    panic("shell returned");
}
