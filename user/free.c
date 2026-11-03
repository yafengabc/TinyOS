#include "lib.h"

/* `free` shows two things.
 *
 * The kernel heap is what the allocator hands out: littlefs's caches and
 * exec() load buffers.  It is a fixed .bss arena, and its size is a board
 * decision (the build passes TOS_HEAP_BYTES; the C3 spends 64 KiB).
 *
 * The memory map is the rest of the answer, and the one people actually
 * want: a chip sold as "400 KB SRAM" does not hand 400 KiB to malloc.  On
 * the C3 the 384 KiB SRAM block starts at 0x40380000, but ~14 KiB at the
 * top belongs to the ROM's download buffers and stack, the kernel image
 * and .bss take ~155 KiB, and three 64 KiB windows are reserved for tasks
 * before the kernel is even started.  Nothing is missing; it is spent on
 * purpose, and this prints where. */

/* printf has no %-10s, so a column of labels is padded by hand. */
static void label(const char *s, int width)
{
    int n = (int)strlen(s);
    puts("  ");
    puts(s);
    for (int i = n; i < width; i++)
        putchar(' ');
}

static void map_row(const char *name, uint32_t bytes)
{
    label(name, 10);
    printf("%9u %9u\n", bytes, bytes / 1024);
}

int main(void)
{
    struct meminfo m;
    if (meminfo(&m) != 0) {
        puts("free: meminfo unavailable\n");
        return 1;
    }

    puts("\n                total      used      free      peak\n");
    label("kernel heap", 10);
    printf("%9u %9u %9u %9u\n", m.total, m.used, m.free, m.peak);

    puts("\n                bytes       KiB\n");
    map_row("SRAM",    m.ram_total);
    map_row("kernel",  m.kernel_used);
    map_row("tasks",   m.app_total);
    map_row("unused",  m.ram_unused);
    return 0;
}
