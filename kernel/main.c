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
    kprintf("ring3/unprivileged ELF loader | littlefs | PIE\n");
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

    heap_init();
    ramdisk_init();
    process_init();

    banner();
    list_files();

    kprintf("\nstarting /bin/sh ...\n\n");
    process_start_shell("bin/sh");
    panic("shell returned");
}
