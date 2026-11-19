#include "procfs.h"

#ifndef TOS_PROC_LITE
#define TOS_PROC_LITE 0
#endif
#include "devfs.h"
#include "heap.h"
#include "console.h"
#include "platform.h"
#include "board_config.h"
#include "process.h"
#include "ramdisk.h"
#include "cpu.h"
#include "hwdev.h"
#include "string.h"

/* Everything under /proc is a snapshot taken at open() time: the text is
 * built once into a heap buffer and read() just copies out of it.  That is
 * the whole implementation - no streaming, no re-entry, no lock held across a
 * copy to user space - and it is why a `cat` here is a single allocation
 * rather than a generator state machine.
 *
 * The largest producer is the process table: every slot can add a row of
 * roughly 50 columns plus a header.  Scale with TOS_MAX_PROCS instead of
 * paying the 16-slot size on an 8-slot board; the 512-byte base covers the
 * other /proc files when no process is live. */
#ifndef TOS_MAX_PROCS
#define TOS_MAX_PROCS 16
#endif
#define PROC_CAP (512 + TOS_MAX_PROCS * 64)

struct pfile {
    char    *buf;
    uint32_t len;
};

extern char __bss_end[];

/* Run-time A/B switch for the CDC no-reader backlog fix.  Defined here (a
 * file every target links) rather than in the board driver so the RV32-only
 * USB code can reference it without weak symbols.  /proc/cdcfix flips it on
 * each read; g_cdc_diag is filled in by the CDC driver on boards that have
 * one and stays all-zero elsewhere. */
volatile uint32_t g_cdc_fix = 1;
volatile uint32_t g_cdc_diag[6];

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

/* Read-only snapshot of the board's CDC TX state.  Reading this must not
 * change anything: it is the device-side criterion for "is output still
 * piling up while nobody reads", which is immune to the 1 ms timing noise
 * that makes smpbench useless on a rebuilt image. */
static uint32_t fmt_cdc_state(char *b, uint32_t cap)
{
    return put(b, cap, 0,
               "cdcfix %u no_reader %u pending %u dropped %u probes %u idle_ms %u line %u\n",
               g_cdc_fix, g_cdc_diag[0], g_cdc_diag[1], g_cdc_diag[2],
               g_cdc_diag[3], g_cdc_diag[4], g_cdc_diag[5]);
}

static uint32_t fmt_cdcstat(char *b, uint32_t cap)
{
    return fmt_cdc_state(b, cap);
}

/* Reading this node flips the CDC fix and reports the state *after* the
 * flip, so one firmware image can compare fix=0 and fix=1 back to back
 * without a rebuild (rebuilding this path perturbs the 1 ms USB phase). */
static uint32_t fmt_cdcfix(char *b, uint32_t cap)
{
    uint32_t irq = plat_irq_save();

    g_cdc_fix = !g_cdc_fix;
    plat_irq_restore(irq);

    return fmt_cdc_state(b, cap);
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

#if !TOS_PROC_LITE
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

/* Linux-shaped per-processor blocks, then the machine-level fields once.
 * The board owns the model and clock, the architecture port owns the ISA
 * string; a board that supplies neither still gets a valid file because
 * the hooks have weak defaults.  cpu_count is the number of CPUs that came
 * online, so an SMP image reports two and a uniprocessor one. */
static uint32_t fmt_cpuinfo(char *b, uint32_t cap)
{
    uint32_t off = 0;
    uint32_t hz = plat_cpu_hz();
    int n = cpu_count;

    if (n < 1)
        n = 1;
    for (int i = 0; i < n; i++) {
        off = put(b, cap, off, "processor\t: %d\n", i);
        off = put(b, cap, off, "model name\t: %s\n", plat_cpu_model());
        off = put(b, cap, off, "isa\t\t: %s\n", plat_cpu_isa());
        if (hz) {
            uint32_t mhz = hz / 1000000u;
            uint32_t frac = (hz % 1000000u) / 1000u;

            off = put(b, cap, off, "cpu MHz\t\t: %u.%03u\n", mhz, frac);
        } else {
            off = put(b, cap, off, "cpu MHz\t\t: unknown\n");
        }
        off = put(b, cap, off, "\n");
    }
    off = put(b, cap, off, "board\t\t: %s\n", plat_board_name());
    off = put(b, cap, off, "cpus\t\t: %d\n", n);
    off = put(b, cap, off, "tick Hz\t\t: %u\n", plat_tick_hz());
    return off;
}
#endif

/* What the port could detect about the primary flash, what the board
 * configured as its build-time figure (the fallback when the port cannot
 * measure), and the filesystem size the two produce. */
/* One USB poll's cost, for sizing a bounded pump (issue #29).  Zero on ports
 * whose controller is interrupt-driven. */
static uint32_t fmt_usbpoll(char *b, uint32_t cap)
{
    uint32_t ns = plat_usb_poll_ns();

    if (!ns)
        return put(b, cap, 0, "usbpoll: not measured\n");
    return put(b, cap, 0, "usbpoll: %u ns/poll (%u polls per 250 ms)\n",
               ns, (uint32_t)(250000000u / ns));
}

/* The console UART exactly as the hardware has it programmed right now.
 *
 * This exists because a port can lose one direction and keep the other: on
 * the RP2350 the boot after a BOOTSEL flash accepts input on UART0 (the
 * shell answers a command sent on the wire) while transmitting nothing, and
 * the same adapter reads the same pins fine on the next power-on.  Without
 * the register state the only way to tell "the board stopped driving TX"
 * from "the adapter stopped receiving" is to guess, and the guess is wrong
 * about half the time.  Word order is fixed by the port; see
 * plat_uart_debug(). */
static uint32_t fmt_uart0(char *b, uint32_t cap)
{
    uint32_t r[12];
    uint32_t off = 0;
    uint32_t any = 0;
    int i;

    plat_uart_debug(r);
    for (i = 0; i < 12; i++)
        any |= r[i];
    if (!any)
        return put(b, cap, 0, "uart0: not available on this port\n");

    off = put(b, cap, off, "uart0 (console, 115200 8N1)\n");
    off = put(b, cap, off, "  cr        %08x  uarten=%u txe=%u rxe=%u\n",
              r[0], r[0] & 1u, (r[0] >> 8) & 1u, (r[0] >> 9) & 1u);
    off = put(b, cap, off, "  fr        %08x  txff=%u txfe=%u busy=%u rxfe=%u\n",
              r[1], (r[1] >> 5) & 1u, (r[1] >> 7) & 1u,
              (r[1] >> 3) & 1u, (r[1] >> 4) & 1u);
    off = put(b, cap, off, "  ibrd      %08x  fbrd=%08x\n", r[2], r[3]);
    off = put(b, cap, off, "  lcr_h     %08x  fen=%u wlen=%u\n",
              r[4], (r[4] >> 4) & 1u, (r[4] >> 5) & 3u);
    off = put(b, cap, off, "  tx  gpio  %08x  pad=%08x (iso=%u od=%u ie=%u)\n",
              r[5], r[6], (r[6] >> 8) & 1u, (r[6] >> 7) & 1u, (r[6] >> 6) & 1u);
    off = put(b, cap, off, "  rx  gpio  %08x  pad=%08x (iso=%u od=%u ie=%u)\n",
              r[7], r[8], (r[8] >> 8) & 1u, (r[8] >> 7) & 1u, (r[8] >> 6) & 1u);
    off = put(b, cap, off, "  clk_peri  ctrl=%08x div=%08x\n", r[9], r[10]);
    off = put(b, cap, off, "  resets    done=%08x\n", r[11]);
    return off;
}

/* Transmit on the console UART and report what the TX pad itself did.
 *
 * `/proc/uart0` proves the *programming* is right; this proves whether any
 * voltage leaves the pin.  0x55 is the payload because it alternates on the
 * wire, so even a handful of samples during one byte land on both levels. */
static uint32_t fmt_uart0tx(char *b, uint32_t cap)
{
    uint32_t r[8];

    if (!plat_uart_tx_probe(r))
        return put(b, cap, 0, "uart0tx: no pad readback on this port\n");
    return put(b, cap, 0,
               "uart0tx: 32 bytes of 0x55 sent\n"
               "  idle gp0     low=%u high=%u   <- must be high, a UART idles high\n"
               "  tx   gp0     low=%u high=%u transitions=%u\n"
               "  controls     gp1(adapter tx) high=%u  gp25(led) high=%u\n"
               "  fr after     %08x (txfe=%u busy=%u)\n"
               "  verdict      %s\n",
               r[4], r[5], r[0], r[1], r[2], r[6], r[7],
               r[3], (r[3] >> 7) & 1u, (r[3] >> 3) & 1u,
               (r[4] || r[5]) == 0u
                   ? "gp0 idles low: the line is held low, not driven high"
                   : ((r[1] > (r[0] + r[1]) / 8u)
                          ? "gp0 swings at both levels - the pin is driven"
                          : "gp0 only spikes above ground - the line is loaded"));
}

/* Drive the TX pad from SIO and read it back.  The companion to
 * /proc/uart0tx: when the UART's frames never reach the pin, this says
 * whether the pad can pull the line high at all. */
static uint32_t fmt_gp0drive(char *b, uint32_t cap)
{
    uint32_t r[6];

    if (!plat_gpio_drive_probe(r))
        return put(b, cap, 0, "gp0drive: not available on this port\n");
    return put(b, cap, 0,
               "gp0drive: pad driven from SIO (uart line briefly off)\n"
               "  driving 1    high=%u low=%u\n"
               "  driving 0    high=%u low=%u\n"
               "  writes landed pin_mux=%08x sio_oe=%08x (want 5 / bit0)\n"
               "  verdict      %s\n",
               r[0], r[1], r[2], r[3], r[4], r[5],
               r[0] ? "the pad pulls the line high - the board can drive it"
                    : "the line stays low even when the pad drives 1");
}

static uint32_t fmt_flash(char *b, uint32_t cap){
#if TOS_HAS_FLASH
    uint32_t configured = TOS_FLASH_BYTES;
    uint32_t offset = TOS_LFS_OFFSET;
#else
    uint32_t configured = 0;
    uint32_t offset = 0;
#endif
    uint32_t off = 0;

    off = put(b, cap, off, "flash\n");
    off = put(b, cap, off, "  detected    %8u bytes\n",
              plat_flash_detected_bytes());
    off = put(b, cap, off, "  configured  %8u bytes\n", configured);
    off = put(b, cap, off, "  lfs_offset  %8u bytes\n", offset);
    off = put(b, cap, off, "  lfs_blocks  %8u\n", plat_lfs_blocks());

    /* A port whose probe failed can explain itself here (RP2040: SSIENR,
     * CTRLR0, SR, RXFLR, drain, exchange, SR after, stage reached).  All
     * zero means no port debug. */
    if (plat_flash_detected_bytes() == 0u) {
        uint32_t dbg[12];
        int i;
        uint32_t any = 0;

        plat_flash_probe_debug(dbg);
        for (i = 0; i < 12; i++)
            any |= dbg[i];
        if (any)
            off = put(b, cap, off,
                      "  probe_dbg   %08x %08x %08x %08x\n"
                      "              %08x %08x %08x %08x\n"
                      "              %08x %08x %08x %08x (last = stage)\n",
                      dbg[0], dbg[1], dbg[2], dbg[3],
                      dbg[4], dbg[5], dbg[6], dbg[7],
                      dbg[8], dbg[9], dbg[10], dbg[11]);
    }
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
    else if (!strcmp(d->name, "proc/cdcfix"))
        n = fmt_cdcfix(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/cdcstat"))
        n = fmt_cdcstat(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/ps"))
        n = process_ps_text(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/execfmt"))
        n = process_execfmt_text(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/flash"))
        n = fmt_flash(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/usbpoll"))
        n = fmt_usbpoll(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/uart0"))
        n = fmt_uart0(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/uart0tx"))
        n = fmt_uart0tx(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/gp0drive"))
        n = fmt_gp0drive(b, PROC_CAP);
#if !TOS_PROC_LITE
    else if (!strcmp(d->name, "proc/mem"))
        n = fmt_mem(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/lfs"))
        n = fmt_lfs(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/stack"))
        n = fmt_stack(b, PROC_CAP);
    else if (!strcmp(d->name, "proc/cpuinfo"))
        n = fmt_cpuinfo(b, PROC_CAP);
#endif
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
    devfs_register("proc/cdcfix", &proc_ops, NULL);
    devfs_register("proc/cdcstat", &proc_ops, NULL);
    devfs_register("proc/ps",     &proc_ops, NULL);
    devfs_register("proc/execfmt", &proc_ops, NULL);
    devfs_register("proc/flash",  &proc_ops, NULL);
    devfs_register("proc/usbpoll", &proc_ops, NULL);
    devfs_register("proc/uart0",   &proc_ops, NULL);
    devfs_register("proc/uart0tx", &proc_ops, NULL);
    devfs_register("proc/gp0drive", &proc_ops, NULL);
#if !TOS_PROC_LITE
    devfs_register("proc/mem",    &proc_ops, NULL);
    devfs_register("proc/lfs",    &proc_ops, NULL);
    devfs_register("proc/stack",  &proc_ops, NULL);
    devfs_register("proc/cpuinfo", &proc_ops, NULL);
#endif
}
