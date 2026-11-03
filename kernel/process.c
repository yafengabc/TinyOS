#include "process.h"
#include "tty.h"
#include "elf.h"
#include "ramdisk.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "lz4.h"
#include "pipe.h"
#include "devfs.h"
#include "platform.h"
#include "cpu.h"
#include "tos.h"

#define MAX_PROCS   16
#define MAX_ARGS    12
#define MAX_FDS     12

/* Both live in the board's layout.c: the size of the window decides what
 * fits, and the port that maps them (rv32 PMP, ARM MPU) needs the same
 * numbers.  process_init() checks the combination. */
#define STACK_BYTES PLAT_STACK_BYTES

/* Byte written over a new task's stack, and the value stack_used() looks for.
 * 0xCC is the usual x86 int3 fill; any constant works, the only requirement
 * being that a program is unlikely to write it as its first stack byte (which
 * would make the measurement read short). */
#define STACK_POISON 0xCC

/* Defined next to process_ps_text(), which is where it is used for display;
 * task_release() needs it too, and proc_t is not a type yet at this point. */
static uint32_t stack_used(const struct proc *p);

/* Left unmapped between the heap and the stack: a heap that runs off its
 * end and a stack that runs off its bottom then fault instead of eating
 * each other.  The size lives in platform.h because the port that maps the
 * hole (rv32 PMP, ARM MPU) needs the same number. */
#define HEAP_GUARD  PLAT_HEAP_GUARD

/* Ticks a task may run before the timer takes the CPU away.  At 100 Hz a
   slice is 50 ms: long enough that switches are noise, short enough that a
   spinning program is invisible. */
#define QUANTUM     5

/* Priority levels, lower number wins.  A child starts at its parent's level
 * and setprio() moves it afterwards; nothing ages, because a level is only
 * starved when somebody leaves a higher-priority task runnable forever -
 * which is what asking for levels means.  Nobody can climb above their own
 * level (see process_setprio), so a program cannot lock the console out:
 * the worst it can do is round-robin, which is what the shell had before. */
#define PRIO_LEVELS 3
#define PRIO_HIGH   0
#define PRIO_NORMAL 1
#define PRIO_LOW    2

enum {
    ST_FREE = 0,
    ST_READY,
    ST_RUNNING,
    ST_STOPPED,     /* ^Z: parked, keeps its frame and window */
    ST_WAIT_CHILD,
    ST_WAIT_INPUT,
    ST_WAIT_SLEEP,
    ST_WAIT_PIPE,
    /* A slot claimed by proc_alloc() and still being filled in.  It has to
     * exist as a state of its own: on SMP two exec()s can be building two
     * tasks at the same time, and a slot that is only "not FREE yet" is
     * what keeps them from choosing the same one.  The scheduler never
     * picks it (it only looks at ST_READY), and proc_create() publishes
     * ST_READY as its last act, under the table lock. */
    ST_NEW,
};

/* What a descriptor points at.  A pipe needs its own type because the two
 * ends behave differently (a read end sees EOF, a write end sees EPIPE), and
 * console descriptors are marked rather than left implicit so dup2() can
 * replace one: fd 1 is FD_CONSOLE until somebody redirects it. */
enum {
    FD_NONE = 0,
    FD_CONSOLE,
    FD_FILE,
    FD_PIPE_R,
    FD_PIPE_W,
    FD_CHRDEV,      /* /dev/... or /proc/...: a table of ops, not a file */
};

typedef struct {
    uint8_t  type;
    int      fh;           /* FD_FILE: littlefs open-file handle */
    uint32_t size;
    uint32_t pos;
    struct pipe *pipe;     /* FD_PIPE_R / FD_PIPE_W */
    const dev_t *dev;      /* FD_CHRDEV: which device this descriptor is on */
    void    *devpriv;      /* FD_CHRDEV: per-open state from ops->open() */
    char    *path;         /* FD_FILE: heap copy, so a child can reopen it */
    uint8_t  writable;     /* FD_FILE: created for writing (reopen as rw) */
} file_t;

/* A descriptor has to be duplicable to be inheritable, and both the child
 * inheritance path and dup2() need exactly the same rules.  One helper, so
 * they cannot disagree about which types are copyable. */
static int  fd_clone(file_t *dst, const file_t *src);
static char *kstrdup(const char *s);

typedef struct proc {
    int      state;
    int      prio;          /* PRIO_*, 0 = highest */
    int      pid;
    int      wait_pid;      /* ST_WAIT_CHILD: which child, -1 for any */
    int      exit_code;
    int      laggards;      /* children that exited without a waiter */
    uint32_t in_buf;        /* ST_WAIT_INPUT: where the line goes */
    uint32_t in_len;
    uint64_t wake_tick;     /* ST_WAIT_SLEEP */
    uint64_t cpu_ms;        /* CPU time actually spent on the CPU */
    uint32_t cpu_ticks;     /* whole ticks spent running, see cpu_ms */

    /* This task's slice of the flat address space: image at the bottom,
     * sbrk heap above it, stack at the top.  wx_start splits the image
     * into non-writable code below and writable data above; win_size is how
     * many bytes the allocator handed out (zero = no window, which is what
     * a free table entry reads as). */
    uint32_t win_base;
    uint32_t win_size;
    uint32_t wx_start;
    uint32_t stack_top;
    uint32_t heap_base;
    uint32_t heap_end;
    uint32_t entry;
    uint32_t user_esp;
    uint32_t brk;

    char     name[10];
    struct proc *parent;
    uint64_t start_ns;      /* uptime when created, for a ps %CPU column */
    int      stop_pending;  /* SIGSTOP arrived: park at the trap exit */
    int      kill_pending;  /* SIGKILL aimed at the task on the CPU: die at
                             * the trap exit, see process_suspend() */

    /* Which CPU is running this task, -1 when it is off-CPU.  Bookkeeping
     * for ps on an SMP build: a task's state says "running" but only the
     * CPU field says whether it is this one or the other.  Maintained by
     * whichever scheduler put it there, and only ever read for display. */
    int      cpu;

    /* Current working directory (always absolute, no trailing slash unless
     * it is the root "/").  A child inherits the parent's cwd at exec time,
     * so a program run from a subdirectory sees the same directory. */
    char     cwd[64];

    /* ST_WAIT_PIPE: which channel we are waiting on, in which direction,
     * and for which user buffer.  The waker completes the transfer on our
     * behalf and writes the byte count into the saved frame - exactly how a
     * console reader is handed its line, and the reason nothing has to be
     * replayed when the task resumes. */
    struct pipe *wpipe;
    int      wdir;          /* 0 = waiting to read, 1 = waiting to write */
    uint32_t wbuf;
    uint32_t wlen;

    /* Everything the task needs while it is not on the CPU. */
    regs_t   saved;

    file_t fds[MAX_FDS];
} proc_t;

static proc_t procs[MAX_PROCS];
static proc_t *init_proc;
static int next_pid = 1;

/* ---- what became per-CPU ---------------------------------------------
 *
 * On one CPU these three were file-static: who is running, how much of its
 * slice is left, and whether the trap exit should switch.  With two cores
 * each needs its own answer - hart 0 running the shell says nothing about
 * what hart 1 is doing - so they moved into cpu_t (kernel/cpu.h) and the
 * file reaches them through these macros.  Every existing use reads exactly
 * as before, which is why this conversion touched almost nothing else.
 *
 * The code they are used from is the same on both sides of the divide: a
 * trap handler runs with interrupts off on its own CPU, so per-CPU state
 * needs no lock.  Only the shared process table does - see proc_lock. */
#define current        (cpu_self()->cur)
#define rr             (cpu_self()->rr)
#define quantum        (cpu_self()->quantum)
#define need_resched   (cpu_self()->need_resched)

/* The process table is shared, and on SMP two CPUs pick from it at the same
 * instant.  One lock over the whole table is the honest choice here: the
 * critical sections are a sixteen-entry scan and a state store, so a finer
 * grain would cost more in bounds checks than it saves in contention.
 *
 * Lock order, the only one in the kernel: proc_lock, then nothing.  Nothing
 * that blocks (console, pipe, timer wait) is ever called while holding it -
 * the scheduler hands the lock back before it idles. */
static spin_t proc_lock = SPIN_INIT;

/* The machine is on its way down: init has exited and the boot CPU is in
 * plat_shutdown().  Without this, another hart that finds an empty run queue
 * in that instant cannot tell "the system is going down" from "the scheduler
 * lost every task", and panics on a machine that is shutting down exactly as
 * asked.  Set (under proc_lock) before init's slot is freed, so a CPU that
 * sees the empty queue is guaranteed to see the flag too. */
static volatile int sys_down;

/* Defined with the rest of the descriptor code at the bottom of the file;
 * task_release() (which runs when a task dies) needs it up here. */
static void fd_close_in(proc_t *p, int fd);

/* One kernel stack per CPU.  The stack start.S switches to before kmain is
 * the same one traps from user mode land on; on an SMP build each hart has
 * its own block and both start.S and CPU_KSTACK_TOP (kernel/cpu.h) work out
 * where it is from the hart id, so there is nothing to initialise.  A trap
 * from user mode always starts at its top and everything it pushes is
 * unwound before the next trap, so the kernel stack carries no state
 * between traps. */
#define KSTACK_TOP CPU_KSTACK_TOP(cpu_id())
#define KSTACK_LOW CPU_KSTACK_LOW(cpu_id())

/* There is no guard page to fault on - no MMU - so running off the end of
 * the kernel stack used to end up as a jump into empty memory: a serial
 * driver that recursed on backspace wrapped 16 KiB and died at
 * EIP=4b976946 with the stack pointer outside RAM.  One word at the bottom
 * is enough to recognise the situation: the stack grows down, so anything
 * that overflows must overwrite it first.  With several CPUs each block
 * gets its own guard word, written by cpu_init(). */
#define KSTACK_GUARD 0x5354414Bu              /* "STAK" */

static void kstack_guard_init(void)
{
    for (int i = 0; i < (int)(MAX_CPUS); i++) {
        if (i < (int)PLAT_MAX_CPUS)
            *(volatile uint32_t *)CPU_KSTACK_LOW(i) = KSTACK_GUARD;
    }
}

static void kstack_check(regs_t *r)
{
    uint32_t sp = (uint32_t)r;

    if (sp < KSTACK_LOW || sp >= KSTACK_TOP)
        panic("kernel stack out of bounds: sp=%x", sp);
    if (*(volatile uint32_t *)KSTACK_LOW != KSTACK_GUARD)
        panic("kernel stack overflow");
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Look a task up by pid.  Out of line on purpose: -O2 turns a sixteen-step
 * loop over a static array into ~300 bytes of unrolled code, and there are
 * two callers (signals, setprio).  One copy is cheaper than both. */
static proc_t *proc_by_pid(int pid) __attribute__((noinline));

static proc_t *proc_by_pid(int pid)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        /* ST_NEW is a slot claimed by an exec() that has not picked a pid
         * yet: it still holds the previous task's number and must not be
         * mistaken for it. */
        if (procs[i].state != ST_FREE && procs[i].state != ST_NEW &&
            procs[i].pid == pid)
            return &procs[i];
    }
    return NULL;
}

/* ---- scheduler ------------------------------------------------------- */

/* A task more important than whoever is on the CPU must not wait for the
 * rest of that slice: need_resched makes the trap exit switch, exactly as
 * an expired quantum would. */
static void preempt_if_higher(proc_t *p)
{
    if (current && current->state == ST_RUNNING && p->prio < current->prio)
        need_resched = 1;
}

/* Hand a blocked task its result and make it runnable.  `val` lands in the
 * saved frame, so the task resumes straight in user mode with that return
 * value - nothing is replayed.
 *
 * The state change is under the table lock because on SMP the waker and the
 * scheduler on another CPU can look at the same task at the same time: one
 * has to win, and it must be the waker (a task the scheduler already saw as
 * READY must not be handed a result afterwards and lose it). */
static void make_ready(proc_t *p, int32_t val)
{
    plat_frame_set_ret(&p->saved, (uint32_t)val);
    spin_lock(&proc_lock);
    p->state = ST_READY;
    spin_unlock(&proc_lock);
    preempt_if_higher(p);
}

static proc_t *sched_pick(void)
{
    for (int lvl = 0; lvl < PRIO_LEVELS; lvl++) {
        for (int i = 1; i <= MAX_PROCS; i++) {
            int idx = (rr + i) % MAX_PROCS;
            if (procs[idx].state == ST_READY && procs[idx].prio == lvl) {
                rr = idx;
                return &procs[idx];
            }
        }
    }
    return NULL;
}

static int proc_waiting(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        int st = procs[i].state;
        if (st == ST_WAIT_CHILD || st == ST_WAIT_INPUT || st == ST_WAIT_SLEEP ||
            st == ST_WAIT_PIPE)
            return 1;
    }
    return 0;
}

/* Is another CPU running something?  On one CPU this is always false and
 * costs a comparison.  With two it is what keeps the idle hart from
 * declaring the kernel dead while its neighbour is happily running the
 * shell: "nobody blocked" does not mean "nothing left to do" once the other
 * hart holds a task in ST_RUNNING, which no other CPU may pick up. */
static int other_cpu_busy(void)
{
    int me = cpu_id();

    for (int i = 0; i < (int)cpu_count && i < (int)(MAX_CPUS); i++) {
        proc_t *p = cpus[i].cur;

        if (i == me || !p)
            continue;
        if (p->state == ST_RUNNING)
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

/* Hand the hardware the current shape of a task's window.  Called on every
 * switch (protection belongs to whoever is on the CPU) and again when sbrk
 * moves the heap bound. */
static void apply_protection(proc_t *p)
{
    plat_task_protect(p->win_base, p->win_base + p->win_size, p->wx_start,
                      p->brk, p->stack_top - STACK_BYTES, p->stack_top);
}

/* Save the outgoing frame and return the frame to resume.  Callers hold the
 * CPU at the top of a trap, so abandoning the kernel stack below us costs
 * nothing: the next trap starts at the top again. */
static regs_t *sched_switch(regs_t *r)
{
    proc_t *prev = current;

    if (prev && prev->state == ST_RUNNING) {
        prev->saved = *r;
        spin_lock(&proc_lock);
        prev->state = ST_READY;
        prev->cpu = -1;
        spin_unlock(&proc_lock);
    }
    /* Somebody asked for this switch; whoever asked (a wake-up, a syscall)
       does not need to ask again once we are here. */
    need_resched = 0;

    for (;;) {
        proc_t *next;
        int waiting;

        /* Picking a task and asking "is anybody still blocked?" have to
         * observe the same instant.  sched_idle() below switches interrupts
         * on and nothing ever switches them off again, so from the first
         * idle onwards this loop runs preemptible.  A tick landing between
         * the scan and the probe then hands a finished console line to a
         * task sched_pick() has already walked past: the scan reports
         * "nothing ready" (it will not look behind rr again this round) and
         * the probe reports "nobody waiting" (the sleeper just became
         * READY), which used to add up to a panic on a machine that had a
         * runnable shell all along.  Closing the window here is enough -
         * the frame the tick wrote is picked up by the next iteration.
         *
         * On SMP the lock does the same job against the other CPU: both the
         * scan and the probe happen inside it, so a task cannot slip from
         * one state to the other between them. */
        plat_irq_disable();
        spin_lock(&proc_lock);
        next = sched_pick();
        waiting = proc_waiting() || other_cpu_busy();
        if (next) {
            next->state = ST_RUNNING;
            next->cpu = cpu_id();
            current = next;
            quantum = QUANTUM;
            spin_unlock(&proc_lock);
            apply_protection(next);
            /* Leave interrupts closed: the caller is trap exit and mret
               restores MIE from MPIE, which is the state we came in with. */
            return &next->saved;
        }
        spin_unlock(&proc_lock);
        if (!waiting) {
            /* Shutting down is not a scheduling failure: the boot CPU is
             * already in plat_shutdown() and this one just waits for the
             * lights to go out. */
            if (sys_down) {
                sched_idle();
                continue;
            }
            panic("no runnable process");
        }
        sched_idle();       /* re-enables interrupts and sleeps until the next tick */
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
    spin_lock(&proc_lock);
    p->state = st;
    p->cpu = -1;
    spin_unlock(&proc_lock);

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

    if (current && current->state == ST_RUNNING) {
        current->cpu_ticks++;
        current->cpu_ms += 1000u / plat_tick_hz();
    }

    /* Console input first: a line finished this tick should reach its
       reader in the same tick.  Echo leaves through the console only if
       nothing was printed during this tick (see console_echo_tick).
     *
     * Only the boot CPU does any of this.  The console is one shared device
     * with one FIFO and one line buffer, and the tty layer is written for a
     * single reader: letting both harts drain the FIFO would split a line
     * between two shells, and letting both hand the same finished line to
     * the same waiter would deliver it twice.  Whoever owns the hardware
     * owns the driver - the same rule the GPIO and ADC layers follow.
     * Sleepers are still woken on every CPU: the deadline is a process
     * property, not a console one. */
    if (cpu_id() == 0) {
        tty_poll();
        console_echo_tick();
        if (tty_is_raw()) {
            /* Raw reader: hand over whatever arrived, a keystroke at a time.
             * An editor wants the byte before the next tick, not after a
             * newline that will never come. */
            if (tty_raw_ready()) {
                for (int i = 0; i < MAX_PROCS; i++) {
                    proc_t *p = &procs[i];
                    if (p->state != ST_WAIT_INPUT)
                        continue;
                    int n = tty_take_raw((char *)p->in_buf, (int)p->in_len);
                    make_ready(p, n);
                    break;
                }
            }
        } else if (tty_line_ready()) {
            for (int i = 0; i < MAX_PROCS; i++) {
                proc_t *p = &procs[i];
                if (p->state != ST_WAIT_INPUT)
                    continue;
                int n = tty_take_line((char *)p->in_buf, (int)p->in_len);
                make_ready(p, n);
                break;
            }
        }

        /* Control characters act on the foreground task - whoever is blocked
           reading the console - which is exactly the job the two shells above
           found.  They are handled after the line hand-over so that ^C on an
           empty prompt cannot be swallowed as input. */
        tty_ctrl_deliver();
    }

    now = plat_ticks();
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];
        if (p->state == ST_WAIT_SLEEP && (int64_t)(now - p->wake_tick) >= 0)
            make_ready(p, 0);
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
    if (need_resched) {
        need_resched = 0;
        return sched_switch(r);
    }
    return r;
}

/* A secondary CPU's first task.  It has no trap frame to switch away from,
 * so it cannot use sched_switch(): it takes one task out of the shared run
 * queue, installs it on itself and drops into user mode.  Everything after
 * that is an ordinary trap on that CPU.
 *
 * Called with interrupts off (start.S has just come up); the caller turns
 * them back on before idling.  Returns NULL when nothing is ready, which on
 * a freshly started CPU is normal - the boot CPU may still be alone. */
proc_t *cpu_take_task(void)
{
    proc_t *p;

    plat_irq_disable();
    spin_lock(&proc_lock);
    p = sched_pick();
    if (p) {
        p->state = ST_RUNNING;
        p->cpu = cpu_id();
        current = p;
        quantum = QUANTUM;
    }
    spin_unlock(&proc_lock);

    if (p)
        apply_protection(p);
    return p;
}

/* See process.h: proc_t is private to this file. */
regs_t *process_saved_frame(struct proc *p)
{
    return &((proc_t *)p)->saved;
}

/* Trap exit for the current task: park it if somebody stopped it while it
 * was running, or finish a kill that had nobody else to run it.  Deferred
 * from process_signal because stopping there would abandon a kernel stack
 * that is still in use - here the frame is already saved and sched_switch()
 * abandons the stack the way every switch does. */
regs_t *process_suspend(regs_t *r)
{
    if (current && current->kill_pending) {
        /* ^C arrived while this very task was on the CPU (an editor drawing
         * its screen spends most of its time there), so process_signal()
         * could not rip out the frame that was running.  It marked the task
         * instead; this is the first safe point to act on it. */
        current->kill_pending = 0;
        kprintf("\n[pid %d] killed\n", current->pid);
        return process_exit(r, 128 + 9);
    }
    if (current && current->stop_pending && current->state == ST_RUNNING) {
        current->stop_pending = 0;
        kprintf("\n[pid %d] stopped\n", current->pid);
        current->state = ST_STOPPED;
        return sched_switch(r);
    }
    return r;
}

/* Cheap sanity check of the task layout, run once at boot.  See the comment
 * on PLAT_STACK_BYTES in platform.h for why each condition is there. */
static void layout_check(void)
{
    uint32_t grain = PLAT_APP_GRAIN;
    uint32_t stk = PLAT_STACK_BYTES;
    uint32_t guard = PLAT_HEAP_GUARD;
    uint32_t area = PLAT_APP_MAX - PLAT_APP_BASE;
    uint32_t min_win;

    if (stk == 0 || (stk & (stk - 1)) != 0)
        panic("task layout: stack is %u bytes, which is not a power of two", stk);
    if (grain % stk != 0)
        panic("task layout: the %u KiB grain is not a multiple of the %u KiB stack, "
              "so stack_low would drift off its own alignment",
              grain / 1024u, stk / 1024u);
    if (PLAT_APP_BASE % grain != 0)
        panic("task layout: user area starts at 0x%x, not on a %u KiB grain boundary",
              PLAT_APP_BASE, grain / 1024u);
    /* The smallest window still has to hold a stack, the guard hole and at
       least one page of image - that is what "a 32 KiB window has 12 KiB
       for the image" means. */
    min_win = ((stk + guard + PLAT_PAGE_SIZE + grain - 1u) / grain) * grain;
    if (min_win > area)
        panic("task layout: a %u KiB window needs %u KiB of user area, and there "
              "are only %u KiB", grain / 1024u, min_win / 1024u, area / 1024u);
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

    /* The task layout is four constants from the board's layout.c, and a
     * board that gets one of them wrong produces symptoms that look like
     * anything but a layout mistake: a stack that faults halfway up, a guard
     * page that is writable, every exec refused as "does not fit".  Check the
     * combination once, here, where it costs nothing. */
    layout_check();

    plat_set_kernel_stack(KSTACK_TOP);
}

int process_current_pid(void)
{
    return current ? current->pid : 0;
}

/* The cwd is per task.  When no task is current (early boot, before the
 * shell starts) the answer is the root, which is also what an uninitialised
 * proc_t reads as after the memset in proc_create(). */
const char *process_cwd_str(void)
{
    return (current && current->cwd[0]) ? current->cwd : "/";
}

void process_set_cwd_str(const char *path)
{
    if (!current)
        return;
    strncpy(current->cwd, path, sizeof(current->cwd) - 1);
    current->cwd[sizeof(current->cwd) - 1] = '\0';
}

int process_console_reader(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == ST_WAIT_INPUT)
            return procs[i].pid;
    }
    return 0;
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
    return addr + len <= current->win_base + current->win_size;
}

/* ---- windows ---------------------------------------------------------
 *
 * A window is sized to the image that will live in it: exec() measures the
 * ELF (span), adds the stack and the guard hole, rounds up to the board's
 * grain and asks for exactly that.  The shell ends up with 32 KiB on a C3
 * instead of the 64 KiB every task used to reserve, and the difference goes
 * to the next program.
 *
 * The live windows are the tasks: each proc_t records its base and size, so
 * there is no second structure to keep in sync (and none to size for boards
 * whose user area is 112 MiB), a task's death merges its gap back for free,
 * and two tasks can never be handed overlapping memory.  The scan is over
 * MAX_PROCS entries, twice per gap - 16 tasks, so "O(n^2)" is 256 compares.
 *
 * Caller holds no lock; everything here runs under the table lock, because
 * an exec() on the other CPU is inserting a window at the same moment.  The
 * window is recorded in the proc_t *inside* that lock - reserving it after
 * the lock is dropped would let two concurrent execs pick the same gap,
 * each seeing a table that does not yet mention the other. */
static int win_alloc(proc_t *p, uint32_t bytes)
{
    uint32_t cand = PLAT_APP_BASE;
    uint32_t end = PLAT_APP_MAX;
    int ok = 0;

    spin_lock(&proc_lock);
    for (;;) {
        uint32_t next = end;

        /* The nearest window at or above `cand` bounds the gap we are
           standing in. */
        for (int i = 0; i < MAX_PROCS; i++) {
            proc_t *q = &procs[i];
            if (q->win_size != 0 && q->win_base >= cand && q->win_base < next)
                next = q->win_base;
        }
        if (next - cand >= bytes) {
            p->win_base = cand;
            p->win_size = bytes;
            ok = 1;
            break;
        }
        if (next >= end)
            break;                       /* no window above: ran out of room */

        /* Too small: step over that window and look at the next gap. */
        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].win_size != 0 && procs[i].win_base == next) {
                cand = procs[i].win_base + procs[i].win_size;
                break;
            }
        }
        if (cand >= end)
            break;
    }
    spin_unlock(&proc_lock);
    return ok ? 0 : -1;
}

/* Longest contiguous free run, for the "no room" message: without it a
 * fragmented area reports "out of task windows" on a machine with 40 KiB
 * free, and the next person looks for a leak. */
static uint32_t win_largest_free(void)
{
    uint32_t cand = PLAT_APP_BASE;
    uint32_t end = PLAT_APP_MAX;
    uint32_t best = 0;

    spin_lock(&proc_lock);
    for (;;) {
        uint32_t next = end;

        for (int i = 0; i < MAX_PROCS; i++) {
            proc_t *q = &procs[i];
            if (q->win_size != 0 && q->win_base >= cand && q->win_base < next)
                next = q->win_base;
        }
        if (next - cand > best)
            best = next - cand;
        if (next >= end)
            break;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].win_size != 0 && procs[i].win_base == next) {
                cand = procs[i].win_base + procs[i].win_size;
                break;
            }
        }
        if (cand >= end)
            break;
    }
    spin_unlock(&proc_lock);
    return best;
}

/* ---- creation -------------------------------------------------------- */

static proc_t *proc_alloc(void)
{
    /* Claim the slot, not just find it: proc_create() then spends a while
     * loading the ELF and building the initial stack, and on SMP a second
     * exec() on the other CPU must not settle on the same table entry while
     * that is happening.  pid is set by the caller; the state is what makes
     * the slot owned, and ST_NEW says "owned, not runnable". */
    proc_t *p = NULL;

    spin_lock(&proc_lock);
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == ST_FREE) {
            p = &procs[i];
            /* Clear it here rather than in proc_create(): the caller used to
             * memset() the whole struct a hundred lines later, which wiped
             * this very claim (ST_FREE is 0) and reopened the race. */
            memset(p, 0, sizeof(*p));
            p->state = ST_NEW;
            p->cpu = -1;
            break;
        }
    }
    spin_unlock(&proc_lock);
    return p;
}

/* Hand a slot back to the table.  Two callers: a failed exec() (a claimed
 * slot that never became a task) and a dying one.  Both have to be under the
 * table lock, because on SMP the other CPU is scanning the table at the same
 * moment - a half-cleared entry can be picked up as a runnable task.
 *
 * Clearing win_size is what frees the memory: the live windows *are* the
 * table (see win_alloc), so a task that stops being a task stops holding
 * address space, and its gap merges with its neighbours on the next scan. */
static void proc_retire(proc_t *p)
{
    spin_lock(&proc_lock);
    p->state = ST_FREE;
    p->pid = 0;
    p->parent = NULL;
    p->cpu = -1;
    p->win_base = 0;
    p->win_size = 0;
    spin_unlock(&proc_lock);
}

/* Where the initial [argc][argv..][NULL] block would start for argc
 * arguments.  build_initial_stack() lays the strings out below
 * stack_top - 256 and this block below them, so the distance to the
 * initial sp is the headroom a start-up frame needs; a program that needs
 * more stack than the window has left must be refused (see process_exec). */
static uint32_t argv_block_start(proc_t *p, int argc)
{
    uint32_t sp = p->stack_top - 256;

    sp = (sp - (uint32_t)(argc + 2) * 4) & ~0xFu;
    return sp;
}

static uint32_t build_initial_stack(proc_t *p, char *const argv[], int argc)
{
    if (argc > MAX_ARGS)
        return 0;

    uint32_t stack_low = p->stack_top - STACK_BYTES;
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

    /* How much of the window will the image itself occupy, and where does
     * its writable half begin? */
    uint32_t link_lo = 0, link_hi = 0, link_entry = 0, link_wx = 0;
    if (elf_probe(data, size, &link_lo, &link_hi, &link_entry, &link_wx) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        ramdisk_release(data);
        proc_retire(p);
        return NULL;
    }
    uint32_t span = align_up(link_hi - link_lo, PLAT_PAGE_SIZE);
    uint32_t app_total = PLAT_APP_MAX - PLAT_APP_BASE;
    if (span + STACK_BYTES + HEAP_GUARD > app_total) {
        kprintf("exec: '%s' needs %u KiB, the whole user area is %u KiB\n",
                path, (span + STACK_BYTES + HEAP_GUARD) / 1024u,
                app_total / 1024u);
        ramdisk_release(data);
        proc_retire(p);
        return NULL;
    }

    /* Size the window to this image and nothing more: a page for what the
       image rounds up to, the stack, the guard hole.  A small program stops
       reserving a big program's share. */
    uint32_t win = align_up(span + STACK_BYTES + HEAP_GUARD, PLAT_APP_GRAIN);
    if (win < PLAT_APP_GRAIN)
        win = PLAT_APP_GRAIN;

    if (win_alloc(p, win) != 0) {
        kprintf("exec: '%s' needs a %u KiB window, the largest free block is %u KiB\n",
                path, win / 1024u, win_largest_free() / 1024u);
        ramdisk_release(data);
        proc_retire(p);
        return NULL;
    }
    uint32_t base = p->win_base;     /* recorded by win_alloc, under the lock */

    /* The loader relocates the position independent (ET_DYN) image to
       `base` and checks every write against the window. */
    elf_set_window(base, base + win);

    uint32_t entry = 0;
    if (elf_load(data, size, base, &entry) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        elf_set_window(0, 0);
        ramdisk_release(data);
        proc_retire(p);
        return NULL;
    }
    elf_set_window(0, 0);
    ramdisk_release(data);

    /* proc_alloc() already cleared this entry and marked it ST_NEW; from
     * here to the publish at the bottom it is "owned and unusable", which is
     * exactly what ST_NEW means.  Start in the root, then inherit the
     * parent's cwd so a program spawned
     * from a subdirectory lists and opens files relative to where the user
     * is, not always the root. */
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    if (current && current->cwd[0]) {
        strncpy(p->cwd, current->cwd, sizeof(p->cwd) - 1);
        p->cwd[sizeof(p->cwd) - 1] = '\0';
    }
    p->cpu = -1;        /* memset() above zeroed it; 0 is a real CPU */
    /* Pid allocation is under the table lock for the same reason the window
     * is: two exec()s on two CPUs would otherwise hand out the same number,
     * and `wait(pid)` would then reap the wrong task. */
    spin_lock(&proc_lock);
    p->pid = next_pid++;
    spin_unlock(&proc_lock);

    /* Descriptors.  0/1/2 are the console by default; everything else is
     * inherited from the parent, which is the whole reason `a | b` works:
     * the shell wires fd 1 to a pipe, then exec()s the child, and the child
     * gets that wiring as part of its own table.  The same is what makes
     * `echo hi > file` work - by the time the child starts, the shell's fd 1
     * is a file, and the child inherits it. */
    for (int i = 0; i < MAX_FDS; i++)
        p->fds[i].type = FD_NONE;
    if (current) {
        for (int i = 0; i < MAX_FDS; i++) {
            /* Console and pipes were always inherited; files and devices
             * joined them when redirection started crossing exec().  The
             * rules live in fd_clone() so dup2() cannot drift from this. */
            if (current->fds[i].type == FD_NONE)
                continue;
            if (fd_clone(&p->fds[i], &current->fds[i]) < 0)
                p->fds[i].type = FD_NONE;
        }
    }
    for (int i = 0; i < 3; i++)
        if (p->fds[i].type == FD_NONE)
            p->fds[i].type = FD_CONSOLE;    /* Children inherit: a task cannot spawn something more important than
       itself, which is what keeps a program from locking the console out. */
    p->prio = current ? current->prio : PRIO_HIGH;
    p->start_ns = plat_uptime_ms();
    /* win_base/win_size were recorded by win_alloc() - under the lock, so
       nobody else can be handed this range while the image loads. */
    p->wx_start = base + (link_wx - link_lo);
    /* The stack occupies the very top of the window, so that its region is
       naturally aligned; the initial frame starts a little below. */
    p->stack_top = base + win;
    p->heap_base = base + span;
    p->heap_end = p->stack_top - STACK_BYTES - HEAP_GUARD;

    /* Fill the stack with a poison byte before anything uses it, so the
       deepest address the program ever wrote to can be recovered later: the
       first byte from the bottom that is no longer 0xCC is the high-water
       mark (see stack_used()).  Done here, before build_initial_stack()
       writes argv on top, so the arguments count as usage - they are memory
       the program can see. */
    memset((void *)(p->stack_top - STACK_BYTES), STACK_POISON, STACK_BYTES);
    p->entry = entry;
    p->brk = p->heap_base;
    set_name(p, path);

    uint32_t sp = build_initial_stack(p, argv, argc);
    if (!sp) {
        kprintf("exec: out of memory\n");
        proc_retire(p);
        return NULL;
    }
    p->user_esp = sp;

    /* The very top of the stack is reserved for the handler path: an
       exception taken by a user task pushes its 8-word hardware frame (and
       then the kernel's 80-byte regs_t) below a stack pointer that can be
       as high as the initial frame.  256 bytes is what that needs, and it
       has to be untouched by ordinary stack use, so a program that uses a
       lot of stack must be refused here instead of faulting on its first
       interrupt.  The guess comes from the image's .bss: the shell keeps
       its line buffer there, at the lowest address .bss is loaded to. */
    uint32_t need = (argv_block_start(p, argc) - sp) + 256u;
    if (link_hi - link_lo > need)
        need = link_hi - link_lo;
    if (sp - need < p->stack_top - STACK_BYTES) {
        kprintf("exec: '%s' needs %u bytes of stack, the window has %u\n",
                path, need, STACK_BYTES);
        proc_retire(p);
        return NULL;
    }

    /* The frame this task will resume on.  Nothing else is needed to start
       it: the trap epilogue restores exactly this. */
    plat_prepare_user_frame(&p->saved, p->entry, p->user_esp);

    /* Publish it.  The state store is the last write and it happens under
       the table lock, so a scheduler on another CPU either sees a free slot
       or a finished task, never a half-built one. */
    spin_lock(&proc_lock);
    p->state = ST_READY;
    spin_unlock(&proc_lock);
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

/* Move a task to another level.  pid 0 means the caller, so a program can
 * nice itself without knowing its pid.  Climbing above the caller's own
 * level is refused rather than policed: with no aging, a program that could
 * promote itself above the shell would own the console, and the only honest
 * way to keep that from happening in ~40 bytes is to not allow it. */
int process_setprio(int pid, int prio)
{
    proc_t *t;

    if (prio < 0 || prio >= PRIO_LEVELS)
        return -1;
    if (current && prio < current->prio)
        return -1;

    if (pid == 0) {
        t = current;
    } else {
        t = proc_by_pid(pid);
    }
    if (!t)
        return -1;

    t->prio = prio;
    if (t != current && t->state == ST_READY)
        preempt_if_higher(t);
    return 0;
}

/* ---- exit ------------------------------------------------------------ */

/* Deepest stack any task has ever used, and which task it was: the record
 * outlives the task, because the programs whose stack use matters most are
 * the ones that finish (a `cat` of a large file, an `edit` save).  Updated
 * where a task dies, while its window is still its own.  A torn read on SMP
 * (two tasks exiting at once) can pair one task's number with another's name;
 * that is a diagnostic, not state anything acts on. */
static struct {
    uint32_t bytes;
    char     name[10];
} stack_peak;

uint32_t process_stack_peak(char *name, uint32_t cap)
{
    uint32_t bytes = stack_peak.bytes;

    if (name && cap) {
        uint32_t i = 0;
        for (; i + 1 < cap && stack_peak.name[i]; i++)
            name[i] = stack_peak.name[i];
        name[i] = '\0';
    }
    return bytes;
}

/* Close a task's files and give back its window.  Shared by process_exit()
 * (the task exits itself) and process_signal() (SIGKILL against somebody
 * else). */
static void task_release(proc_t *p)
{
    uint32_t used = stack_used(p);

    if (used > stack_peak.bytes) {
        stack_peak.bytes = used;
        for (uint32_t i = 0; i + 1 < sizeof(stack_peak.name); i++) {
            stack_peak.name[i] = p->name[i];
            if (!p->name[i])
                break;
        }
        stack_peak.name[sizeof(stack_peak.name) - 1] = '\0';
    }

    for (int i = 0; i < MAX_FDS; i++)
        fd_close_in(p, i);
    /* A full-screen program that dies (^C, a crash, a plain exit) must not
     * leave the console in raw mode: the shell's next getline() would then
     * return single bytes and every command would look like one letter. */
    if (tty_is_raw())
        tty_set_raw(0);
    /* The window goes back with the table entry, in proc_retire(). */
}

/* Hand a dead task's exit code to whoever waits for it: an actual waiter
 * gets it in its saved frame, otherwise it is remembered for the next
 * wait().  Children of the dead task go to its parent, or to init. */
static void reap_into_parent(proc_t *p, int code)
{
    proc_t *parent = p->parent;

    if (parent) {
        if (parent->state == ST_WAIT_CHILD &&
            (parent->wait_pid == p->pid || parent->wait_pid < 0)) {
            make_ready(parent, code);
        } else {
            parent->exit_code = code;
            parent->laggards++;
        }
    }

    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != ST_FREE && procs[i].parent == p)
            procs[i].parent = parent ? parent : init_proc;
    }
}

/* Retire a task that is not the caller: the caller keeps the CPU, the
 * victim's frame is never resumed again.  Used by SIGKILL; the init task
 * cannot be retired this way (it is what shuts the machine down). */
static void task_kill(proc_t *p, int code)
{
    task_release(p);
    reap_into_parent(p, code);
    proc_retire(p);
}

regs_t *process_exit(regs_t *r, int code)
{
    proc_t *p = current;
    if (!p)
        panic("no current process");

    task_release(p);
    reap_into_parent(p, code);

    int is_init = (p == init_proc);
    if (is_init) {
        /* Raise the flag *before* the slot goes back to the table: a CPU
         * that sees an empty run queue must be able to see it, and the only
         * way to guarantee that without a shared clock is to make the flag
         * visible first. */
        spin_lock(&proc_lock);
        sys_down = 1;
        spin_unlock(&proc_lock);
    }

    proc_retire(p);
    current = NULL;

    if (is_init) {
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

/* ---- signals --------------------------------------------------------- */

/* SIGKILL acts on the target immediately, from whatever context sent it: a
 * task killed by ^C must not get another tick of CPU.  SIGSTOP is only a
 * request - the task has to stop at a boundary where its own kernel stack
 * is not in use, and the only such boundary this kernel has is the trap
 * exit, so the flag is checked there (process_suspend).
 *
 * A task cannot rip out its own frame: when the target is the task on the
 * CPU - which is exactly the case for ^C in a full screen program, because
 * the tick that notices the key usually lands while the program is drawing -
 * the signal is recorded and process_suspend() acts on it at the next trap
 * exit.  Refusing outright (the old behaviour) lost that ^C entirely. */
int process_signal(int pid, int sig)
{
    proc_t *victim = proc_by_pid(pid);

    if (!victim || victim == init_proc)
        return -1;

    if (victim == current) {
        if (sig == SIGKILL)
            victim->kill_pending = 1;
        else if (sig == SIGSTOP)
            victim->stop_pending = 1;
        return 0;
    }

    if (sig == SIGKILL) {
        kprintf("\n[pid %d] killed\n", pid);
        /* The caller keeps the CPU; the victim simply never runs again.
           Its exit code reaches a waiting parent through laggards. */
        task_kill(victim, 137);
        return 0;
    }

    if (sig == SIGSTOP) {
        victim->stop_pending = 1;
        /* A task blocked in the kernel (a sleeper, a console reader, a
           parent waiting for a child) is already off the CPU and only
           re-enters it through the trap exit that wakes it, which is where
           process_suspend parks it.  A task that is NOT on the CPU and not
           blocked is one waiting in the run queue: park it now, there is
           no kernel stack to protect. */
        if (victim->state == ST_READY) {
            victim->state = ST_STOPPED;
            victim->stop_pending = 0;
            kprintf("\n[pid %d] stopped\n", pid);
        }
    } else if (sig == SIGCONT) {
        victim->stop_pending = 0;
        if (victim->state == ST_STOPPED) {
            victim->state = ST_READY;
            preempt_if_higher(victim);
            kprintf("[pid %d] continued\n", pid);
        }
    } else {
        return -1;
    }
    return 0;
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
    /* The heap bound is part of the protection: without this the new pages
       would stay unreachable for the task that just asked for them. */
    apply_protection(current);
    return old;
}

/* Fixed width: the kernel formatter has no %-5s, and adding one would cost
 * more than padding five strings. */
static const char *state_name(int st)
{
    switch (st) {
    case ST_RUNNING:    return "run  ";
    case ST_READY:      return "rdy  ";
    case ST_STOPPED:    return "stop ";
    case ST_WAIT_CHILD: return "wait ";
    case ST_WAIT_INPUT: return "tty  ";
    case ST_WAIT_SLEEP: return "sleep";
    case ST_WAIT_PIPE:  return "pipe ";
    case ST_NEW:        return "new  ";
    default:            return "free ";
    }
}

/* How deep into its stack a task has ever been, in bytes.
 *
 * exec() fills the stack with STACK_POISON before the program gets to run, so
 * the first byte still holding the poison, counting up from the bottom of the
 * stack, marks the deepest address anything has written: everything above it
 * has been touched at least once and nothing below it ever was.  Returned as
 * the distance from that address to stack_top, which is what a program would
 * have used had it grown down from the first instruction.
 *
 * Reading a live task's stack from another CPU is safe for the same reason
 * reading its registers is: it is a read of memory that task owns and nobody
 * else writes, and the worst outcome of racing it is a value one context
 * switch stale.  A task that turned out to be bigger than the poison run
 * reads as full - the measurement can only be short if a program writes 0xCC
 * as its lowest stack byte, which is why STACK_POISON is not 0x00. */
static uint32_t stack_used(const proc_t *p)
{
    const uint8_t *low;
    uint32_t i = 0;

    if (p->win_size == 0 || p->stack_top == 0 || STACK_BYTES == 0)
        return 0;
    low = (const uint8_t *)(p->stack_top - STACK_BYTES);
    while (i < STACK_BYTES && low[i] == STACK_POISON)
        i++;
    return STACK_BYTES - i;
}

/* Format the process table into `buf`, returning the bytes written.
 *
 * Both `ps` and /proc/ps come through here.  The alternative - a kprintf
 * version for the command and a separate text version for the file - is
 * exactly how a /proc view ends up missing a column the command has, and
 * then someone debugs the file instead of the kernel.
 *
 * Column notes: the window address says nothing here (every task has the same
 * one, slots are process.c's business) and a raw tick count made CPU time
 * look tiny: one tick is 10 ms of CPU but the task runs for a 50 ms slice
 * including the timer's own work.  cpums is the time spent on the CPU, cpu%
 * its share of the machine since the task started.  The `c` column is the CPU
 * running it right now ("-" when it is not on one): on a two-hart build it is
 * what makes two tasks both saying "run" readable, and it is how a program
 * checks that it migrated.  `stk` is the stack high-water mark in KiB with
 * one decimal - the number that says whether the board's PLAT_STACK_BYTES is
 * the right size rather than a guess. */
uint32_t process_ps_text(char *buf, uint32_t cap)
{
    uint32_t off;
    uint64_t now = plat_uptime_ms();
    int n;

    if (!buf || cap < 64)
        return 0;

    n = ksnprintf(buf, cap, " pid ppid prio state  c cpu%% cpums wink stk  name\n");
    if (n < 0)
        return 0;
    off = (uint32_t)n;

    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];
        char cpubuf[4];
        uint32_t age, busy, pct10, stk10;

        if (p->state == ST_FREE)
            continue;

        if (p->cpu < 0 || p->state != ST_RUNNING) {
            cpubuf[0] = '-';
            cpubuf[1] = '\0';
        } else {
            cpubuf[0] = (char)('0' + (p->cpu % 10));
            cpubuf[1] = '\0';
        }

        age  = (uint32_t)(now - p->start_ns);
        busy = (uint32_t)p->cpu_ms;
        /* Tenths of a percent with 32-bit arithmetic only (rv32 has no
           64-bit divide); saturate rather than wrap. */
        pct10 = 0;
        if (age > 0) {
            uint32_t t = (uint32_t)(busy / age);
            pct10 = t * 1000u;
            pct10 += ((busy % age) * 1000u) / age;
        }
        if (pct10 > 9999u)
            pct10 = 9999u;

        /* Tenths of a KiB: a task whose deepest call used 1.4 KiB is the
           common case here, and whole KiB would round it to a 1 or a 2 that
           says nothing about whether the board's stack size is generous. */
        stk10 = stack_used(p) * 10u / 1024u;

        if (off >= cap - 1)
            break;
        n = ksnprintf(buf + off, cap - off,
                      "%4d %4d %4d %s %2s %4u.%u %5u %4u %4u.%u  %s\n", p->pid,
                      p->parent ? p->parent->pid : 0, p->prio,
                      state_name(p->state), cpubuf, pct10 / 10u, pct10 % 10u,
                      busy / 1000u, p->win_size / 1024u,
                      stk10 / 10u, stk10 % 10u, p->name);
        if (n > 0)
            off += (uint32_t)n;
    }
    return off;
}

int process_dump(void)
{
    /* A stack buffer, not kmalloc: this runs from a syscall, the table is
     * bounded at 16 rows (~950 bytes with the stack column), and 1.5 KiB is
     * small next to the 32 KiB kernel stack.  kmalloc could fail here, and
     * "ps stops working when the heap is full" is a poor answer to "what is
     * using the heap". */
    char buf[1536];
    uint32_t n = process_ps_text(buf, sizeof(buf));
    int count = 0;

    if (n)
        console_write(buf, n);

    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != ST_FREE)
            count++;
    return count;
}

/* ---- file descriptors ------------------------------------------------- */

static file_t *fd_slot(proc_t *p, int fd)
{
    if (!p || fd < 0 || fd >= MAX_FDS)
        return NULL;
    return &p->fds[fd];
}

/* The descriptor number *is* the table index.  open() starts at 3: 0/1/2
 * belong to the console and a program that opens a file must not be handed
 * its own stdout by mistake. */
static int fd_alloc(proc_t *p)
{
    for (int i = 3; i < MAX_FDS; i++)
        if (p->fds[i].type == FD_NONE)
            return i;
    return -1;
}

/* A path has to survive the descriptor if a child is ever going to reopen it
 * (see the inheritance path in proc_create).  Copying is the only option: the
 * caller's buffer is a syscall argument that is gone by the time a child
 * exists.  Failure is not fatal - the file simply becomes non-inheritable,
 * which is what every file used to be. */
static char *kstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = kmalloc(n);

    if (p)
        memcpy(p, s, n);
    return p;
}

/* Make `dst` a second, independent instance of `src`.
 *
 * The rule that shapes this function: a descriptor owns something, and two
 * descriptors must never own it twice.  A pipe end is shared by refcount; a
 * file handle is *reopened* because it carries a read position and exactly
 * one close; a character device gets its own per-open state so /proc buffers
 * are not freed twice.
 *
 * Returns 0, or -1 when the source cannot be duplicated (a file whose path
 * was not saved, because the heap was full when it was opened). */
static int fd_clone(file_t *dst, const file_t *src)
{
    if (!dst || !src)
        return -1;
    memset(dst, 0, sizeof(*dst));

    switch (src->type) {
    case FD_CONSOLE:
        dst->type = FD_CONSOLE;
        return 0;

    case FD_PIPE_R:
    case FD_PIPE_W:
        *dst = *src;
        if (src->type == FD_PIPE_R)
            pipe_ref_reader(src->pipe);
        else
            pipe_ref_writer(src->pipe);
        return 0;

    case FD_FILE: {
        int h;

        if (!src->path)
            return -1;
        /* Reopen with the same access the original had.  A read-only handle
         * would make `echo hi > f` fail in the child, which is the one case
         * inheritance exists for now. */
        h = src->writable ? ramdisk_open_rw(src->path)
                          : ramdisk_open(src->path);
        if (h < 0)
            return -1;
        /* The new handle starts at 0; the descriptor it replaces was
         * somewhere else.  Without this a child reading a file it inherited
         * halfway through would start over. */
        if (ramdisk_seek(h, src->pos) < 0) {
            ramdisk_close(h);
            return -1;
        }
        dst->type     = FD_FILE;
        dst->fh       = h;
        dst->size     = ramdisk_size(h);
        dst->pos      = src->pos;
        dst->writable = src->writable;
        dst->path     = kstrdup(src->path);
        return 0;
    }

    case FD_CHRDEV: {
        void *priv = src->dev->priv;

        if (src->dev->ops->open && src->dev->ops->open(src->dev, &priv) < 0)
            return -1;
        dst->type    = FD_CHRDEV;
        dst->dev     = src->dev;
        dst->devpriv = priv;
        dst->pos     = src->pos;
        return 0;
    }

    default:
        return -1;
    }
}

int process_fd_open(const char *path)
{
    const dev_t *d;
    file_t *f;
    int fd, h;

    if (!current)
        return -1;

    /* Device paths are not on the filesystem, so this lookup failing is the
     * normal case for every plain file open.  Trying devices first costs one
     * strcmp chain and keeps ramdisk.c from needing to know about /dev. */
    d = devfs_lookup(path);
    if (d) {
        void *priv = d->priv;

        fd = fd_alloc(current);
        if (fd < 0)
            return -1;
        if (d->ops->open && d->ops->open(d, &priv) < 0) {
            current->fds[fd].type = FD_NONE;
            return -1;
        }
        f = &current->fds[fd];
        f->type    = FD_CHRDEV;
        f->dev     = d;
        f->devpriv = priv;
        f->pos     = 0;
        f->size    = 0;
        return fd;
    }

    h = ramdisk_open(path);
    if (h < 0)
        return -1;

    fd = fd_alloc(current);
    if (fd < 0) {
        ramdisk_close(h);
        return -1;
    }
    f = &current->fds[fd];
    f->type = FD_FILE;
    f->fh   = h;
    f->size = ramdisk_size(h);
    f->pos  = 0;
    f->path = kstrdup(path);
    return fd;
}

int process_fd_create(const char *path)
{
    file_t *f;
    int fd, h;

    if (!current)
        return -1;

    /* Creating a device node is a boot-time act (devfs_register), not
     * something a task does by opening a path for writing. */
    if (devfs_lookup(path))
        return -1;

    h = ramdisk_create(path);
    if (h < 0)
        return -1;

    fd = fd_alloc(current);
    if (fd < 0) {
        ramdisk_close(h);
        return -1;
    }
    f = &current->fds[fd];
    f->type = FD_FILE;
    f->fh   = h;
    f->size = ramdisk_size(h);
    f->pos  = 0;
    f->path = kstrdup(path);
    f->writable = 1;            /* a child inheriting this must be able to write */
    return fd;
}

/* Hand a pipe to a sleeping task: whoever moved data completes the transfer
 * on the waiter's behalf, so nothing has to be replayed when it resumes (the
 * same trick the console line hand-over uses).  One delivery per event is
 * enough - the next operation wakes the next waiter. */
static void pipe_wake(struct pipe *p, int dir)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *w = &procs[i];
        int n;

        if (w->state != ST_WAIT_PIPE || w->wpipe != p || w->wdir != dir)
            continue;

        if (dir == 0) {
            n = pipe_read(p, (void *)w->wbuf, w->wlen);
            if (n == PIPE_EAGAIN)
                return;             /* still empty, keep waiting */
        } else {
            n = pipe_write(p, (const void *)w->wbuf, w->wlen);
            if (n == PIPE_EAGAIN)
                return;             /* still full, keep waiting */
            if (n == PIPE_EPIPE) {
                /* Every reader closed while we waited: fail the write
                 * instead of parking it forever. */
                w->wpipe = NULL;
                make_ready(w, (uint32_t)-1);
                continue;
            }
        }
        w->wpipe = NULL;
        make_ready(w, n);           /* n == 0 is EOF and is delivered too */
        return;
    }
}

static regs_t *block_on_pipe(regs_t *r, struct pipe *p, int dir,
                             uint32_t buf, uint32_t len)
{
    proc_t *me = current;

    me->saved = *r;
    plat_frame_set_ret(&me->saved, (uint32_t)-1);
    me->wpipe = p;
    me->wdir  = dir;
    me->wbuf  = buf;
    me->wlen  = len;
    me->state = ST_WAIT_PIPE;
    return sched_switch(r);
}

/* One read on fd 0 with the console attached: a line at a time in cooked
 * mode, a byte at a time in raw mode (an editor draws its own screen and
 * cannot wait for a newline). */
static regs_t *console_read(regs_t *r, uint32_t ptr, uint32_t len)
{
    if (tty_is_raw()) {
        uint32_t room = len < 128 ? len : 128;
        if (!tty_raw_ready())
            return process_block_read(r, ptr, room);
        return done(r, (uint32_t)tty_take_raw((char *)ptr, (int)room));
    }

    uint32_t room = len < TTY_LINE_MAX ? len : TTY_LINE_MAX;
    if (!tty_line_ready())
        return process_block_read(r, ptr, room);
    return done(r, (uint32_t)tty_take_line((char *)ptr, (int)room));
}

regs_t *process_read(regs_t *r, int fd, uint32_t ubuf, uint32_t len)
{
    proc_t *me = current;
    file_t *f;

    if (!me)
        return done(r, (uint32_t)-1);
    if (len == 0)
        return done(r, 0);

    f = fd_slot(me, fd);
    if (!f || f->type == FD_NONE)
        return done(r, (uint32_t)-1);

    switch (f->type) {
    case FD_CONSOLE:
        /* Only fd 0 reads the keyboard; fd 1/2 are outputs. */
        if (fd != 0)
            return done(r, (uint32_t)-1);
        return console_read(r, ubuf, len);

    case FD_FILE: {
        uint32_t avail;
        int n;

        if (f->pos >= f->size)
            return done(r, 0);
        avail = f->size - f->pos;
        if (len > avail)
            len = avail;
        n = ramdisk_read(f->fh, (void *)ubuf, len);
        if (n > 0)
            f->pos += (uint32_t)n;
        return done(r, (uint32_t)n);
    }

    case FD_CHRDEV: {
        int n;

        /* The driver owns the text form and the EOF question; this layer
         * only keeps the position.  A device that has nothing more to say
         * returns 0, which is indistinguishable from a file at EOF - which
         * is exactly what `cat` needs. */
        if (!f->dev->ops->read)
            return done(r, (uint32_t)-1);
        n = f->dev->ops->read(f->devpriv, f->pos, (void *)ubuf, len);
        if (n > 0)
            f->pos += (uint32_t)n;
        return done(r, (uint32_t)n);
    }

    case FD_PIPE_R: {
        int n = pipe_read(f->pipe, (void *)ubuf, len);
        if (n == PIPE_EAGAIN)
            return block_on_pipe(r, f->pipe, 0, ubuf, len);
        /* Room appeared: a writer may be parked waiting for it. */
        if (n > 0)
            pipe_wake(f->pipe, 1);
        return done(r, (uint32_t)n);
    }

    default:
        return done(r, (uint32_t)-1);
    }
}

regs_t *process_write(regs_t *r, int fd, uint32_t ubuf, uint32_t len)
{
    proc_t *me = current;
    file_t *f;

    if (!me)
        return done(r, (uint32_t)-1);
    if (len == 0)
        return done(r, 0);

    f = fd_slot(me, fd);
    if (!f || f->type == FD_NONE)
        return done(r, (uint32_t)-1);

    switch (f->type) {
    case FD_CONSOLE:
        console_write((const char *)ubuf, len);
        return done(r, len);

    case FD_FILE: {
        int n = ramdisk_write(f->fh, (const void *)ubuf, len);
        if (n > 0)
            f->size += (uint32_t)n;
        return done(r, (uint32_t)n);
    }

    case FD_CHRDEV: {
        int n;

        if (!f->dev->ops->write)
            return done(r, (uint32_t)-1);
        n = f->dev->ops->write(f->devpriv, f->pos, (const void *)ubuf, len);
        if (n > 0)
            f->pos += (uint32_t)n;
        return done(r, (uint32_t)n);
    }

    case FD_PIPE_W: {
        int n = pipe_write(f->pipe, (const void *)ubuf, len);
        if (n == PIPE_EAGAIN)
            return block_on_pipe(r, f->pipe, 1, ubuf, len);
        if (n == PIPE_EPIPE)
            return done(r, (uint32_t)-1);
        pipe_wake(f->pipe, 0);      /* data for a sleeping reader */
        return done(r, (uint32_t)n);
    }

    default:
        return done(r, (uint32_t)-1);
    }
}

/* Close one descriptor of `p`.  Shared by the syscall (the caller is `p`)
 * and by task_release (a dead task, which is not necessarily current). */
static void fd_close_in(proc_t *p, int fd)
{
    file_t *f = fd_slot(p, fd);

    if (!f || f->type == FD_NONE)
        return;

    switch (f->type) {
    case FD_FILE:
        ramdisk_close(f->fh);
        if (f->path)
            kfree(f->path);
        break;

    case FD_CHRDEV:
        if (f->dev->ops->close)
            f->dev->ops->close(f->devpriv);
        break;

    case FD_PIPE_R: {
        struct pipe *pp = f->pipe;
        if (pipe_drop_reader(pp)) {
            kfree(pp);              /* no references left at all */
        } else if (pipe_readers(pp) == 0) {
            /* Writers waiting for space have nobody to write to now. */
            pipe_wake(pp, 1);
        }
        break;
    }

    case FD_PIPE_W: {
        struct pipe *pp = f->pipe;
        if (pipe_drop_writer(pp)) {
            kfree(pp);
        } else if (pipe_writers(pp) == 0) {
            /* Readers blocked on an empty pipe must see EOF, not hang. */
            pipe_wake(pp, 0);
        }
        break;
    }

    default:
        break;
    }

    memset(f, 0, sizeof(*f));
    /* 0, 1 and 2 are never really closed: "close the redirect" means "back
     * to the console", which is what the shell relies on after a pipeline
     * puts fd 1 on a pipe. */
    if (fd < 3)
        f->type = FD_CONSOLE;
}

int process_fd_close(int fd)
{
    if (!current)
        return -1;
    if (fd < 0 || fd >= MAX_FDS)
        return -1;
    fd_close_in(current, fd);
    return 0;
}

/* Configuration on an open descriptor: the half of device access that is
 * neither a read nor a write.  Only character devices have an answer; for a
 * file or a pipe "set the mode" has no meaning, and -1 says so instead of
 * silently succeeding. */
int process_ioctl(int fd, int cmd, int arg)
{
    file_t *f;

    if (!current)
        return -1;
    f = fd_slot(current, fd);
    if (!f || f->type != FD_CHRDEV || !f->dev->ops->ioctl)
        return -1;
    return f->dev->ops->ioctl(f->devpriv, cmd, arg);
}

int process_pipe(int *fds)
{
    proc_t *me = current;
    struct pipe *p;
    int r, w;

    if (!me || !fds)
        return -1;

    p = pipe_create();
    if (!p)
        return -1;

    r = fd_alloc(me);
    if (r < 0) {
        kfree(p);
        return -1;
    }
    me->fds[r].type = FD_PIPE_R;    /* claim the slot before the next scan */
    me->fds[r].pipe = p;
    pipe_ref_reader(p);

    w = fd_alloc(me);
    if (w < 0) {
        me->fds[r].type = FD_NONE;
        kfree(p);
        return -1;
    }
    me->fds[w].type = FD_PIPE_W;
    me->fds[w].pipe = p;
    pipe_ref_writer(p);

    fds[0] = r;
    fds[1] = w;
    return 0;
}

int process_dup2(int oldfd, int newfd)
{
    proc_t *me = current;
    file_t *o, *n, tmp;

    if (!me)
        return -1;
    o = fd_slot(me, oldfd);
    n = fd_slot(me, newfd);
    if (!o || !n || o->type == FD_NONE)
        return -1;
    if (oldfd == newfd)
        return newfd;

    /* Build the copy first: if it fails (no descriptors, no heap) the target
     * must be left alone rather than closed for nothing.  What "copy" means
     * per type is fd_clone()'s business - the same rules a child inherits
     * by. */
    if (fd_clone(&tmp, o) < 0)
        return -1;

    fd_close_in(me, newfd);
    *n = tmp;
    return newfd;
}

void process_start_shell(const char *path)
{
    proc_t *p = proc_create(path, NULL, 0);
    if (!p)
        panic("cannot load initial program '%s'", path);

    init_proc = p;

    /* Claim it inside the table lock.  proc_create() published it as READY,
     * and on an SMP build the other hart's idle loop is already scanning for
     * exactly that: between the publish and this line it could take the
     * shell, and then both CPUs would be running the same task on two
     * different stacks.  Setting RUNNING here is the same "publish" as the
     * one in sched_switch(), just from the boot CPU's own hand. */
    spin_lock(&proc_lock);
    p->state = ST_RUNNING;
    p->cpu = cpu_id();
    current = p;
    quantum = QUANTUM;
    spin_unlock(&proc_lock);

    plat_set_kernel_stack(KSTACK_TOP);
    apply_protection(p);

    /* Straight into the frame proc_create prepared. */
    plat_enter_user(&p->saved);
}
