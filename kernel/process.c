#include "process.h"
#include "tty.h"
#include "elf.h"
#include "ramdisk.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "lz4.h"
#include "platform.h"

#define MAX_PROCS   16
#define MAX_ARGS    12
#define STACK_PAGES 8
#define MAX_FDS     8

/* Ticks a task may run before the timer takes the CPU away.  At 100 Hz a
   slice is 50 ms: long enough that switches are noise, short enough that a
   spinning program is invisible. */
#define QUANTUM     5
#define MAX_WINDOWS 32

enum {
    ST_FREE = 0,
    ST_READY,
    ST_RUNNING,
    ST_WAIT_CHILD,
    ST_WAIT_INPUT,
    ST_WAIT_SLEEP,
};

typedef struct {
    int      used;
    int      fh;           /* littlefs open-file handle, -1 when unused */
    uint32_t size;
    uint32_t pos;
} file_t;

typedef struct proc {
    int      state;
    int      pid;
    int      slot;          /* window slot, -1 when none */
    int      wait_pid;      /* ST_WAIT_CHILD: which child, -1 for any */
    int      exit_code;
    int      laggards;      /* children that exited without a waiter */
    uint32_t in_buf;        /* ST_WAIT_INPUT: where the line goes */
    uint32_t in_len;
    uint64_t wake_tick;     /* ST_WAIT_SLEEP */
    uint32_t cpu_ticks;     /* ticks spent running, for ps */

    /* This task's slice of the flat address space: image at the bottom,
     * sbrk heap above it, stack at the top. */
    uint32_t win_base;
    uint32_t stack_top;
    uint32_t heap_base;
    uint32_t heap_end;
    uint32_t entry;
    uint32_t user_esp;
    uint32_t brk;

    char     name[10];
    struct proc *parent;

    /* Everything the task needs while it is not on the CPU. */
    regs_t   saved;

    file_t fds[MAX_FDS];
} proc_t;

static proc_t procs[MAX_PROCS];
static proc_t *current;
static proc_t *init_proc;
static int next_pid = 1;

/* Windows are handed out from a bitmap: with background jobs, siblings
 * coexist and are no longer strictly LIFO. */
static uint32_t win_used;
static int win_slots;

/* Round-robin cursor and the "slice is over" flag the trap exit checks. */
static int rr;
static int quantum = QUANTUM;
static volatile int need_resched;

/* One kernel stack only: the stack start.S switches to before kmain is the
 * same one traps from user mode land on.  A trap from user mode always
 * starts at its top and everything it pushes is unwound before the next
 * trap, so the kernel stack carries no state between traps. */
extern uint8_t __kernel_stack[];
extern uint8_t __kernel_stack_top[];
#define KSTACK_TOP ((uint32_t)__kernel_stack_top)

/* There is no guard page to fault on - no MMU - so running off the end of
 * the kernel stack used to end up as a jump into empty memory: a serial
 * driver that recursed on backspace wrapped 16 KiB and died at
 * EIP=4b976946 with the stack pointer outside RAM.  One word at the bottom
 * is enough to recognise the situation: the stack grows down, so anything
 * that overflows must overwrite it first. */
#define KSTACK_GUARD 0x5354414Bu              /* "STAK" */

static void kstack_guard_init(void)
{
    ((volatile uint32_t *)__kernel_stack)[0] = KSTACK_GUARD;
}

static void kstack_check(regs_t *r)
{
    uint32_t sp = (uint32_t)r;

    if (sp < (uint32_t)__kernel_stack || sp >= KSTACK_TOP)
        panic("kernel stack out of bounds: sp=%x", sp);
    if (*(volatile uint32_t *)__kernel_stack != KSTACK_GUARD)
        panic("kernel stack overflow");
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* ---- scheduler ------------------------------------------------------- */

static proc_t *sched_pick(void)
{
    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (rr + i) % MAX_PROCS;
        if (procs[idx].state == ST_READY) {
            rr = idx;
            return &procs[idx];
        }
    }
    return NULL;
}

static int proc_waiting(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        int st = procs[i].state;
        if (st == ST_WAIT_CHILD || st == ST_WAIT_INPUT || st == ST_WAIT_SLEEP)
            return 1;
    }
    return 0;
}

/* Nothing runnable: let the CPU rest until an interrupt.  The timer tick is
 * what makes progress here - it wakes sleepers and hands finished console
 * lines to whoever is blocked reading. */
static void sched_idle(void)
{
    plat_irq_enable();
    plat_cpu_idle();
}

/* Save the outgoing frame and return the frame to resume.  Callers hold the
 * CPU at the top of a trap, so abandoning the kernel stack below us costs
 * nothing: the next trap starts at the top again. */
static regs_t *sched_switch(regs_t *r)
{
    proc_t *prev = current;

    if (prev && prev->state == ST_RUNNING) {
        prev->saved = *r;
        prev->state = ST_READY;
    }

    for (;;) {
        proc_t *next = sched_pick();
        if (next) {
            next->state = ST_RUNNING;
            current = next;
            quantum = QUANTUM;
            return &next->saved;
        }
        if (proc_waiting()) {
            sched_idle();
            continue;
        }
        panic("no runnable process");
    }
}

/* Give up the CPU until somebody completes our request. */
static regs_t *block(regs_t *r, int st)
{
    proc_t *p = current;

    p->saved = *r;
    /* Woken by something unexpected?  Better a failed syscall than the
       syscall number handed back as a result. */
    plat_frame_set_ret(&p->saved, (uint32_t)-1);
    p->state = st;

    return sched_switch(r);
}

static regs_t *done(regs_t *r, int32_t val)
{
    plat_frame_set_ret(r, (uint32_t)val);
    return r;
}

void process_tick(regs_t *r)
{
    uint64_t now;

    /* Cheapest moment to notice a blown kernel stack: the tick runs no
       matter what the task is doing, and r points at the trap frame on
       that very stack. */
    kstack_check(r);

    if (current && current->state == ST_RUNNING)
        current->cpu_ticks++;

    /* Console input first: a line finished this tick should reach its
       reader in the same tick.  Echo leaves through the console only if
       nothing was printed during this tick (see console_echo_tick). */
    tty_poll();
    console_echo_tick();
    if (tty_line_ready()) {
        for (int i = 0; i < MAX_PROCS; i++) {
            proc_t *p = &procs[i];
            if (p->state != ST_WAIT_INPUT)
                continue;
            int n = tty_take_line((char *)p->in_buf, (int)p->in_len);
            plat_frame_set_ret(&p->saved, (uint32_t)n);
            p->state = ST_READY;
            break;
        }
    }

    now = plat_ticks();
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];
        if (p->state == ST_WAIT_SLEEP && (int64_t)(now - p->wake_tick) >= 0) {
            plat_frame_set_ret(&p->saved, 0);
            p->state = ST_READY;
        }
    }

    /* Preempt only a task the timer interrupted in user mode: a tick that
       lands inside a kernel call must let that call finish, because the
       kernel stack below it is not resumable. */
    if (!current || !plat_fault_is_user(r))
        return;
    if (quantum > 0)
        quantum--;
    if (quantum == 0)
        need_resched = 1;
}

regs_t *process_resched(regs_t *r)
{
    if (!need_resched)
        return r;
    need_resched = 0;
    return sched_switch(r);
}

void process_init(void)
{
    memset(procs, 0, sizeof(procs));
    current = NULL;
    init_proc = NULL;
    next_pid = 1;
    rr = 0;
    quantum = QUANTUM;
    need_resched = 0;

    /* Before the first tick: kmain runs with interrupts off up to here. */
    kstack_guard_init();

    win_used = 0;
    win_slots = (int)((PLAT_APP_MAX - PLAT_APP_BASE) / PLAT_APP_WINDOW);
    if (win_slots > MAX_WINDOWS)
        win_slots = MAX_WINDOWS;

    plat_set_kernel_stack(KSTACK_TOP);
}

int process_current_pid(void)
{
    return current ? current->pid : 0;
}

/* Validate a user pointer against the current task's window.  Without an
 * MMU this is a range check, not a security boundary: see PLATFORM-DESIGN. */
int process_user_ok(uint32_t addr, uint32_t len)
{
    if (!current)
        return 0;
    if (len == 0)
        return 1;
    if (addr < current->win_base)
        return 0;
    if (addr + len < addr)
        return 0;
    return addr + len <= current->win_base + PLAT_APP_WINDOW;
}

/* ---- windows --------------------------------------------------------- */

static int win_alloc(void)
{
    for (int i = 0; i < win_slots; i++) {
        if (!(win_used & (1u << i))) {
            win_used |= 1u << i;
            return i;
        }
    }
    return -1;
}

static void win_free(int slot)
{
    if (slot >= 0 && slot < win_slots)
        win_used &= ~(1u << slot);
}

/* ---- creation -------------------------------------------------------- */

static proc_t *proc_alloc(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == ST_FREE)
            return &procs[i];
    }
    return NULL;
}

static uint32_t build_initial_stack(proc_t *p, char *const argv[], int argc)
{
    if (argc > MAX_ARGS)
        return 0;

    uint32_t stack_low = p->stack_top - STACK_PAGES * PLAT_PAGE_SIZE;
    /* Leave a little headroom at the very top: some programs treat argv[]
       as writable storage and grow it upwards through the initial frame. */
    uint32_t sp = p->stack_top - 256;
    uint32_t uargv[MAX_ARGS];

    /* Strings first, at the top: the program's stack grows down from the
       [argc][argv] block below them, so it can never clobber its own
       arguments. */
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        if (sp < stack_low + 64 + len)
            return 0;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        uargv[i] = sp;
    }

    /* [argc][argv[0..argc-1]][NULL], with argc 16-byte aligned: the RISC-V
       psABI (and the i386 one for SSE users) requires the stack to be
       aligned at a call boundary, and crt0 makes its first call right after
       reading these words.  Rounding down keeps the block below the
       strings. */
    sp = (sp - (uint32_t)(argc + 2) * 4) & ~0xFu;
    if (sp < stack_low)
        return 0;

    for (int i = 0; i < argc; i++)
        memcpy((void *)(sp + 4u + (uint32_t)i * 4), &uargv[i], 4);
    uint32_t zero = 0;
    memcpy((void *)(sp + 4u + (uint32_t)argc * 4), &zero, 4);

    uint32_t argc_word = (uint32_t)argc;
    memcpy((void *)sp, &argc_word, 4);

    return sp;
}

static void set_name(proc_t *p, const char *path)
{
    const char *base = path;

    for (const char *s = path; *s; s++) {
        if (*s == '/')
            base = s + 1;
    }
    int i = 0;
    while (base[i] && i < (int)sizeof(p->name) - 1) {
        p->name[i] = base[i];
        i++;
    }
    p->name[i] = '\0';
}

static proc_t *proc_create(const char *path, char *const argv[], int argc)
{
    uint8_t *data = NULL;
    uint32_t size = 0;

    if (!ramdisk_find(path, &data, &size)) {
        kprintf("exec: '%s' not found\n", path);
        return NULL;
    }

    /* Programs are stored LZ4-compressed.  Decompress into a fresh buffer,
       then hand the plain ELF to the loader. */
    uint32_t raw_size = 0;
    if (lz4_wrap_info(data, size, &raw_size)) {
        uint8_t *raw = kmalloc(raw_size ? raw_size : 1);
        if (!raw) {
            kprintf("exec: out of memory decompressing '%s'\n", path);
            ramdisk_release(data);
            return NULL;
        }
        int n = lz4_decompress_block(data + LZ4_WRAP_HEADER, size - LZ4_WRAP_HEADER,
                                     raw, raw_size);
        ramdisk_release(data);
        if (n < 0 || (uint32_t)n != raw_size) {
            kprintf("exec: '%s' is corrupt (bad LZ4 block)\n", path);
            kfree(raw);
            return NULL;
        }
        data = raw;
        size = raw_size;
    }

    proc_t *p = proc_alloc();
    if (!p) {
        kprintf("exec: process table full\n");
        ramdisk_release(data);
        return NULL;
    }

    /* How much of the window will the image itself occupy? */
    uint32_t link_lo = 0, link_hi = 0, link_entry = 0;
    if (elf_probe(data, size, &link_lo, &link_hi, &link_entry) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        ramdisk_release(data);
        return NULL;
    }
    uint32_t span = align_up(link_hi - link_lo, PLAT_PAGE_SIZE);
    uint32_t stack_bytes = STACK_PAGES * PLAT_PAGE_SIZE;
    if (span + stack_bytes + 4096u > PLAT_APP_WINDOW) {
        kprintf("exec: '%s' does not fit a %u KiB task window\n",
                path, PLAT_APP_WINDOW / 1024u);
        ramdisk_release(data);
        return NULL;
    }

    int slot = win_alloc();
    if (slot < 0) {
        kprintf("exec: out of task windows\n");
        ramdisk_release(data);
        return NULL;
    }
    uint32_t base = PLAT_APP_BASE + (uint32_t)slot * PLAT_APP_WINDOW;

    /* The loader relocates the position independent (ET_DYN) image to
       `base` and checks every write against the window. */
    elf_set_window(base, base + PLAT_APP_WINDOW);

    uint32_t entry = 0;
    if (elf_load(data, size, base, &entry) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        elf_set_window(0, 0);
        ramdisk_release(data);
        win_free(slot);
        return NULL;
    }
    elf_set_window(0, 0);
    ramdisk_release(data);

    memset(p, 0, sizeof(*p));
    p->slot = slot;
    p->pid = next_pid++;
    p->win_base = base;
    p->stack_top = base + PLAT_APP_WINDOW - 256;
    p->heap_base = base + span;
    p->heap_end = p->stack_top - stack_bytes;
    p->entry = entry;
    p->brk = p->heap_base;
    set_name(p, path);

    uint32_t sp = build_initial_stack(p, argv, argc);
    if (!sp) {
        kprintf("exec: out of memory\n");
        win_free(slot);
        return NULL;
    }
    p->user_esp = sp;

    /* The frame this task will resume on.  Nothing else is needed to start
       it: the trap epilogue restores exactly this. */
    plat_prepare_user_frame(&p->saved, p->entry, p->user_esp);
    p->state = ST_READY;
    return p;
}

int process_exec(regs_t *r, const char *path, char *const argv[], int argc)
{
    if (!current)
        return -1;

    proc_t *p = proc_create(path, argv, argc);
    if (!p)
        return -1;

    /* exec spawns; it does not run the child here.  The parent keeps its
       slice and calls wait() when it wants the result. */
    p->parent = current;
    return (int)p->pid;
}

/* ---- exit ------------------------------------------------------------ */

regs_t *process_exit(regs_t *r, int code)
{
    proc_t *p = current;
    if (!p)
        panic("no current process");

    /* close any files the process still had open */
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i].used)
            ramdisk_close(p->fds[i].fh);
    }

    proc_t *parent = p->parent;

    /* Hand the exit code to the parent: an actual waiter gets it in its
       saved frame, otherwise it is remembered for the next wait(). */
    if (parent) {
        if (parent->state == ST_WAIT_CHILD &&
            (parent->wait_pid == p->pid || parent->wait_pid < 0)) {
            plat_frame_set_ret(&parent->saved, (uint32_t)code);
            parent->state = ST_READY;
        } else {
            parent->exit_code = code;
            parent->laggards++;
        }
    }

    /* Orphans outlive us: hand them to our parent, or to init if we are the
       last link in the chain. */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != ST_FREE && procs[i].parent == p)
            procs[i].parent = parent ? parent : init_proc;
    }

    win_free(p->slot);
    p->state = ST_FREE;
    p->pid = 0;
    p->parent = NULL;
    current = NULL;

    if (p == init_proc) {
        kprintf("\ninit process exited with code %d - shutting down\n", code);
        plat_shutdown();
    }

    return sched_switch(r);
}

regs_t *process_wait(regs_t *r, int pid)
{
    proc_t *p = current;

    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *c = &procs[i];
        if (c->state == ST_FREE || c->parent != p)
            continue;
        if (pid >= 0 && c->pid != pid)
            continue;
        p->wait_pid = pid;
        return block(r, ST_WAIT_CHILD);
    }

    /* No such child: either it already exited (we remember its code, but
       not which of several children it was) or it never existed. */
    if (p->laggards > 0) {
        p->laggards--;
        return done(r, p->exit_code);
    }
    return done(r, -1);
}

regs_t *process_yield(regs_t *r)
{
    return sched_switch(r);
}

regs_t *process_sleep(regs_t *r, uint32_t ms)
{
    uint32_t hz = plat_tick_hz();
    uint32_t ticks;

    /* Split the multiply: RV32 has no 64-bit divide and a helper for it
       would cost more than this clamp.  An hour is long enough. */
    if (ms > 3600000u)
        ms = 3600000u;
    ticks = (ms / 1000u) * hz + ((ms % 1000u) * hz) / 1000u;

    if (ticks == 0)
        return done(r, 0);

    current->wake_tick = plat_ticks() + ticks;
    return block(r, ST_WAIT_SLEEP);
}

regs_t *process_block_read(regs_t *r, uint32_t ubuf, uint32_t len)
{
    current->in_buf = ubuf;
    current->in_len = len;
    return block(r, ST_WAIT_INPUT);
}

/* ---- misc ------------------------------------------------------------ */

uint32_t process_sbrk(uint32_t bytes)
{
    if (!current)
        return 0;

    uint32_t old = current->brk;
    if (bytes == 0)
        return old;

    uint32_t newbrk = old + bytes;
    if (newbrk < current->heap_base || newbrk > current->heap_end)
        return 0;

    current->brk = newbrk;
    return old;
}

/* Fixed width: the kernel formatter has no %-5s, and adding one would cost
 * more than padding five strings. */
static const char *state_name(int st)
{
    switch (st) {
    case ST_RUNNING:    return "run  ";
    case ST_READY:      return "rdy  ";
    case ST_WAIT_CHILD: return "wait ";
    case ST_WAIT_INPUT: return "tty  ";
    case ST_WAIT_SLEEP: return "sleep";
    default:            return "free ";
    }
}

int process_dump(void)
{
    int n = 0;

    kprintf(" pid state window      ticks  name\n");
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];
        if (p->state == ST_FREE)
            continue;
        kprintf("%4d %s 0x%08x %6u  %s\n", p->pid, state_name(p->state),
                p->win_base, p->cpu_ticks, p->name);
        n++;
    }
    return n;
}

int process_fd_open(const char *path)
{
    if (!current)
        return -1;

    int h = ramdisk_open(path);
    if (h < 0)
        return -1;

    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i].used) {
            current->fds[i].used = 1;
            current->fds[i].fh = h;
            current->fds[i].size = ramdisk_size(h);
            current->fds[i].pos = 0;
            return 3 + i;
        }
    }
    ramdisk_close(h);
    return -1;
}

int process_fd_create(const char *path)
{
    if (!current)
        return -1;

    int h = ramdisk_create(path);
    if (h < 0)
        return -1;

    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i].used) {
            current->fds[i].used = 1;
            current->fds[i].fh = h;
            current->fds[i].size = ramdisk_size(h);
            current->fds[i].pos = 0;
            return 3 + i;
        }
    }
    ramdisk_close(h);
    return -1;
}

int process_fd_read(int fd, void *buf, uint32_t len)
{
    if (!current || fd < 3)
        return -1;

    int i = fd - 3;
    if (i >= MAX_FDS || !current->fds[i].used)
        return -1;

    file_t *f = &current->fds[i];
    if (f->pos >= f->size)
        return 0;

    uint32_t avail = f->size - f->pos;
    if (len > avail)
        len = avail;

    int n = ramdisk_read(f->fh, buf, len);
    if (n > 0)
        f->pos += (uint32_t)n;
    return n;
}

int process_fd_write(int fd, const void *buf, uint32_t len)
{
    if (!current || fd < 3)
        return -1;

    int i = fd - 3;
    if (i >= MAX_FDS || !current->fds[i].used)
        return -1;

    file_t *f = &current->fds[i];
    int n = ramdisk_write(f->fh, buf, len);
    if (n > 0)
        f->size += (uint32_t)n;
    return n;
}

void process_fd_close(int fd)
{
    if (!current || fd < 3)
        return;
    int i = fd - 3;
    if (i < MAX_FDS && current->fds[i].used) {
        ramdisk_close(current->fds[i].fh);
        memset(&current->fds[i], 0, sizeof(file_t));
    }
}

void process_start_shell(const char *path)
{
    proc_t *p = proc_create(path, NULL, 0);
    if (!p)
        panic("cannot load initial program '%s'", path);

    init_proc = p;
    p->state = ST_RUNNING;
    current = p;
    plat_set_kernel_stack(KSTACK_TOP);

    /* Straight into the frame proc_create prepared. */
    plat_enter_user(&p->saved);
}
