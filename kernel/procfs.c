#include "procfs.h"
#include "devfs.h"
#include "heap.h"
#include "console.h"
#include "platform.h"
#include "process.h"
#include "ramdisk.h"
#include "string.h"

/* Everything under /proc is a snapshot taken at open() time: the text is
 * built once into a heap buffer and read() just copies out of it.  That is
 * the whole implementation - no streaming, no re-entry, no lock held across a
 * copy to user space - and it is why a `cat` here is a single allocation
 * rather than a generator state machine.
 *
 * 1 KiB is sized for the largest producer (the process table: 16 rows of ~50
 * columns plus a header).  A caller that opens all three at once costs 3 KiB
 * of the 32 KiB heap, which is why they are snapshots and not files on disk. */
#define PROC_CAP 1536

struct pfile {
    char    *buf;
    uint32_t len;
};

extern char __bss_end[];

/* Append formatted text, returning the new offset.  kvsnprintf is what the
 * kernel already prints with, so /proc output and the boot log cannot drift
 * apart in their number formatting. */
static uint32_t put(char *b, uint32_t cap, uint32_t off, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n;

    if (off >= cap - 1)
        return cap - 1;
    __builtin_va_start(ap, fmt);
    n = kvsnprintf(b + off, (size_t)(cap - off), fmt, ap);
    __builtin_va_end(ap);
    if (n < 0)
        return off;
    off += (uint32_t)n;
    if (off > cap - 1)
        off = cap - 1;
    return off;
}

static uint32_t fmt_uptime(char *b, uint32_t cap)
{
    uint32_t ms = (uint32_t)plat_uptime_ms();

    /* Seconds with milliseconds, the shape programs expect to parse. */
    return put(b, cap, 0, "%u.%03u\n", ms / 1000u, ms % 1000u);
}

/* printf has no %-10s, so labels are padded by hand (same reason user/free.c
 * pads by hand). */
static uint32_t put_label(char *b, uint32_t cap, uint32_t off, const char *s,
                          int width)
{
    int n = (int)strlen(s);

    off = put(b, cap, off, "  %s", s);
    for (int i = n; i < width; i++)
        off = put(b, cap, off, " ");
    return off;
}

static uint32_t fmt_mem(char *b, uint32_t cap)
{
    uint32_t off = 0;
    uint32_t total = heap_arena_bytes();
    uint32_t used  = heap_used_bytes();
    uint32_t peak  = heap_peak_bytes();

    /* The machine, not just the arena: this is the map `free` explains. */
    uint32_t ram_total   = PLAT_RAM_TOP - PLAT_RAM_BASE;
    uint32_t kernel_used = (uint32_t)__bss_end - PLAT_RAM_BASE;
    uint32_t app_total   = PLAT_APP_MAX - PLAT_APP_BASE;
    uint32_t ram_unused  = (kernel_used + app_total < ram_total)
                           ? ram_total - kernel_used - app_total : 0;

    off = put(b, cap, off, "                total      used      free      peak\n");
    off = put_label(b, cap, off, "kernel heap", 10);
    off = put(b, cap, off, "%9u %9u %9u %9u\n", total, used, total - used,
              peak);

    off = put(b, cap, off, "\n                bytes       KiB\n");
    off = put_label(b, cap, off, "SRAM", 10);
    off = put(b, cap, off, "%9u %9u\n", ram_total, ram_total / 1024u);
    off = put_label(b, cap, off, "kernel", 10);
    off = put(b, cap, off, "%9u %9u\n", kernel_used, kernel_used / 1024u);
    off = put_label(b, cap, off, "tasks", 10);
    off = put(b, cap, off, "%9u %9u\n", app_total, app_total / 1024u);
    off = put_label(b, cap, off, "unused", 10);
    off = put(b, cap, off, "%9u %9u\n", ram_unused, ram_unused / 1024u);
    return off;
}

/* Block-device call counters.  Read them around one operation to see where
 * the time goes: on a 3 MiB write the C3 reports ~5,900 progs of 512 bytes
 * and ~740 block erases, which is why the wall clock is dominated by the
 * erase time and not by how the writes are batched. */
static uint32_t fmt_lfs(char *b, uint32_t cap)
{
    struct lfs_io_stats s;
    uint32_t off = 0;

    ramdisk_io_stats(&s);
    off = put(b, cap, off, "lfs block device\n");
    off = put(b, cap, off, "  reads     %8u (%u sectors)\n",
              s.reads, s.read_sectors);
    off = put(b, cap, off, "  progs     %8u (%u bytes)\n",
              s.progs, s.prog_bytes);
    off = put(b, cap, off, "  erases    %8u\n", s.erases);
    return off;
}

/* The stack question, answered with a number instead of a guess: the board
 * default (PLAT_STACK_BYTES) is what a task gets unless its image carries a
 * TOS_STACK() note or it faults past the bottom and has its stack doubled
 * (process_stack_grow).  It is the biggest single line item in a window, so
 * if the deepest stack any program has ever used is a tenth of it, that tenth
 * is what the default should be - or what that one program should ask for, if
 * it cannot rely on growing.  Reported in KiB with one decimal because the
 * interesting values here are 0.6 and 1.2, not 0 and 1. */
static uint32_t fmt_stack(char *b, uint32_t cap)
{
    char name[12];
    uint32_t used = process_stack_peak(name, sizeof(name));
    uint32_t d10 = used * 10u / 1024u;
    uint32_t off = 0;

    off = put(b, cap, off, "stack high-water mark\n");
    off = put(b, cap, off, "  peak    %3u.%u KiB  by %s\n",
              d10 / 10u, d10 % 10u, used ? name : "-");
    off = put(b, cap, off, "  default %3u KiB, or what the image asks, grown on a fault\n",
              PLAT_STACK_BYTES / 1024u);
    return off;
}

static int proc_open(const dev_t *d, void **priv)
{
    char *b;
    uint32_t n;
    struct pfile *p;

    b = kmalloc(PROC_CAP);
    if (!b)
        return -1;

    if (!strcmp(d->name, "proc/uptime"))
        n = fmt_uptime(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/ps"))
        n = process_ps_text(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/mem"))
        n = fmt_mem(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/lfs"))
        n = fmt_lfs(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/stack"))
        n = fmt_stack(b, PROC_CAP);
    else {
        kfree(b);
        return -1;
    }

    p = kmalloc(sizeof(*p));
    if (!p) {
        kfree(b);
        return -1;
    }
    p->buf = b;
    p->len = n;
    *priv = p;
    return 0;
}

static int proc_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    struct pfile *p = priv;
    uint32_t n;

    if (!p || off >= p->len)
        return 0;               /* EOF: this is what ends a `cat` */
    n = p->len - off;
    if (n > len)
        n = len;
    memcpy(buf, p->buf + off, n);
    return (int)n;
}

static int proc_write(void *priv, uint32_t off, const void *buf, uint32_t len)
{
    /* Kernel state is written through syscalls, not by echoing at a file.
     * The sysfs model would allow it; this one says no, and says so. */
    (void)priv; (void)off; (void)buf; (void)len;
    return -1;
}

static void proc_close(void *priv)
{
    struct pfile *p = priv;

    if (!p)
        return;
    kfree(p->buf);
    kfree(p);
}

static const struct dev_ops proc_ops = {
    .open  = proc_open,
    .read  = proc_read,
    .write = proc_write,
    .close = proc_close,
};

void procfs_init(void)
{
    devfs_register("proc/uptime", &proc_ops, NULL);
    devfs_register("proc/ps",     &proc_ops, NULL);
    devfs_register("proc/mem",    &proc_ops, NULL);
    devfs_register("proc/lfs",    &proc_ops, NULL);
    devfs_register("proc/stack",  &proc_ops, NULL);
}
