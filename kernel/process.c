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
#include "mailbox.h"
#include "platform.h"
#include "cpu.h"
#include "tos.h"

/* Per-target: sizeof(proc_t) is 856 bytes on the C3 (measured with
 * -fdump-record-layouts: fds[12] alone is 432, the saved trap frame 144,
 * cwd 64), so procs[8] is 6,848 B of .bss, not the 11,648 a 16-slot table
 * used to cost.  max_procs is a menuconfig knob: 4 is the floor for a shell
 * plus one child, 16 keeps the historical default for boards that do not
 * override it. */
#ifndef TOS_MAX_PROCS
#define TOS_MAX_PROCS 16
#endif
#define MAX_PROCS   TOS_MAX_PROCS
#define MAX_ARGS    12
#define MAX_FDS     12

/* Unwaited exit codes a parent remembers (see proc_t.lag_pid).  Costs
 * LAG_MAX * 8 bytes per table slot; eight is what a test bench that spawns
 * one child per CPU times two needs, and the shell needs two. */
#define LAG_MAX     8

/* The board's default stack size, from layout.c.  It is what a task gets
 * when its image does not ask for anything else; a program can ask with
 * TOS_STACK() (see elf_tos_note) and the answer lives in proc_t.stack_size,
 * because one board now runs tasks with different stacks at the same time.
 * process_init() checks that the default still works with the grain and the
 * guard; exec() checks each request against the same rules. */
#define DEFAULT_STACK PLAT_STACK_BYTES

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

/* Some PMP implementations (Hazard3 on the RP2350) can only describe a
 * naturally aligned power-of-two range with one entry, and cannot add a
 * second region for the code/data split.  On those boards every task window
 * is rounded up to a power of two and its base is aligned to that size, so
 * one NAPOT entry isolates the task from the kernel and from every other
 * task.  The cost is that the code/data/guard split inside the window is
 * not enforced there; the build defines this for rp2350rv only. */
#ifndef TOS_PMP_POW2_WINDOWS
#define TOS_PMP_POW2_WINDOWS 0
#endif

/* Ticks a task may run before the timer takes the CPU away.  One tick at
   1000 Hz (main.c) makes a slice 1 ms.

   The slice is a fairness knob, not a latency knob: a task that blocks is
   woken by the interrupt that satisfied it, and a task above the current one
   preempts at the next trap exit.  What the slice bounds is how long an
   already-runnable task at the same level waits, and that grows with the
   number of them - measured on the C3 with scripts/esp-c3/schedlat.py, the
   prompt behind no, one, two and three CPU-bound `spin` tasks came back in
   31, 92, 199 and 293 ms at a 50 ms slice, and in 31, 31, 31, 31 ms at 1 ms
   (the 31 ms floor is the console path, not the scheduler).

   The tick, not the slice, is what a sampler needs: at 1000 Hz a sleep(1)
   loop samples every millisecond exactly - user/samp.c reports 1000 of 1000
   samples at 1 ms - and with two same-priority CPU-bound tasks beside it the
   worst gap is 2 ms, against 11 ms with a 5 ms slice.

   What that costs, measured as the same ALU work in three builds of the same
   tree: 100 Hz/1 tick 30101 ms, 1000 Hz/5 ticks 30586 ms, 1000 Hz/1 tick
   31059 ms.  So a 1000 Hz tick is 1.6% of the machine and the 1 ms slice
   another 1.6%, both only while a task is CPU-bound.  Skipping the no-op
   "switch" to the task already running recovered 0.03%, which is why
   sched_switch() still looks the way it always did: at this rate the price
   is fetching the tick path's code, not the bookkeeping in it. */
#define QUANTUM     1

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
    ST_WAIT_THREAD, /* TOS_KTHREADS: a join() waiting for a sibling  */
    ST_KT_WAIT,     /* kernel thread blocked by the WiFi OSI layer */
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

    /* Children that exited before anybody waited for them, as (pid, code)
     * pairs.  A single slot plus a counter is not enough: `piperace 2 64`
     * spawns four children and they all finish before the parent's first
     * wait(), so every code but the last was overwritten and the parent was
     * told -1 for the rest (OPEN-ITEMS §24).  Beyond LAG_MAX unwaited exits
     * the oldest is lost and wait() reports -1 for it - a program that
     * leaves eight dead children behind has a bug of its own. */
    int      lag_pid[LAG_MAX];
    int      lag_code[LAG_MAX];
    uint8_t  lag_n;
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
    uint8_t  is_ktask;      /* runs in M-mode on its own stack, no window */
    uint8_t  kstack_slot;   /* index into the ktask stack pool          */
    uint32_t win_base;
    uint32_t win_size;
    uint32_t wx_start;
    uint32_t stack_top;
    uint32_t stack_size;    /* this task's stack right now: the board's
                             * default, its own TOS_STACK() request, or what
                             * a stack fault grew it to (see
                             * process_stack_grow) */
    uint32_t heap_base;
    uint32_t heap_end;
    uint32_t entry;
    uint32_t user_esp;
    uint32_t brk;

    char     name[10];
    struct proc *parent;

    /* Kernel threads (TOS_KTHREADS): a thread is a proc_t slot that points
     * at its process leader and owns no window.  Code, data, heap, fds and
     * cwd belong to the group; the slot carries only the thread's saved
     * frame, its user stack and its scheduling state.  A leader has
     * leader == NULL, which is also what makes it a window owner. */
    struct proc *leader;
    int      tgid;          /* thread group id: the leader's pid */
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

    /* Everything the task needs while it is not on the CPU.  A kernel
     * thread's live frame is on its own stack (kframe); saved is only the
     * initial bootstrap frame, because the generic M-mode restore derives
     * sp from the frame address itself. */
    regs_t   saved;
    regs_t  *kframe;

    file_t fds[MAX_FDS];
} proc_t;

static proc_t procs[MAX_PROCS];
static proc_t *init_proc;
static int next_pid = 1;

/* Trap entry cannot trust mstatus.MPP to tell it where it came from: mret
 * always writes MPP=U, so a kernel thread entered through a trap frame would
 * look like a user task on its *next* trap and the entry would switch to the
 * kernel stack while the frame is on the ktask stack.  This flag is set by
 * the scheduler whenever a kernel thread is current; start.S selects the
 * current-stack path from it, and trap_handler forces the saved frame's
 * MPP back to M before mret. */
volatile int ktask_running;
/* Set by the scheduler immediately before it returns a ktask's first frame;
 * the bootstrap stub in start.S reads it to get off the frame's stack and
 * onto the ktask's own stack before calling any C code. */
volatile uint32_t ktask_entry_sp;

/* Kernel-thread stacks.  Four slots of 6 KiB; see the layout comment in
 * board/esp32c3/layout.c for the 160 KiB kernel-region budget.  The ktask
 * machinery is RV32-only: it rides the M-mode trap path and start.S. */
#if defined(__riscv)
#define KTASK_SLOTS       6
#define KTASK_STACK_BYTES 6144u
static uint8_t ktask_stacks[KTASK_SLOTS][KTASK_STACK_BYTES]
    __attribute__((aligned(16)));
static uint32_t ktask_stack_used;

void ktask_trampoline(void (*fn)(void *), void *arg);
static void ktask_exit(void) __attribute__((noreturn));
extern void ktask_start_stub(void);
#endif

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

/* Checked forms used on the exec path: every number below comes from an
 * untrusted envelope, and an unchecked wrap here turns an undersized window
 * into an out-of-bounds decompressor write. */
static int add_u32_ok(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a > 0xFFFFFFFFu - b)
        return -1;
    *out = a + b;
    return 0;
}

static int align_up_ok(uint32_t v, uint32_t a, uint32_t *out)
{
    uint32_t rounded;

    if (a == 0 || (a & (a - 1u)) != 0)
        return -1;
    if (v > 0xFFFFFFFFu - (a - 1u))
        return -1;
    rounded = (v + a - 1u) & ~(a - 1u);
    if (rounded < v)
        return -1;
    *out = rounded;
    return 0;
}

#if TOS_PMP_POW2_WINDOWS
/* Smallest power of two >= v, never below one grain.  A window must be a
 * power of two so a single NAPOT PMP entry covers it and nothing else; see
 * TOS_PMP_POW2_WINDOWS at the top of this file. */
static uint32_t win_pow2(uint32_t v)
{
    uint32_t p = PLAT_APP_GRAIN;

    while (p < v && p < PLAT_APP_MAX - PLAT_APP_BASE)
        p <<= 1;
    return p;
}
#endif

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

int process_pid_exists(int pid)
{
    uint32_t irq = plat_irq_save();
    proc_t *p;

    spin_lock(&proc_lock);
    p = proc_by_pid(pid);
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);
    return p != NULL;
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
    /* Interrupts off, not just the lock: a waker in system-call context has
     * them on, and a tick landing between the acquire and the release calls
     * this again on the same CPU, spinning on a lock it already holds. */
    uint32_t irq = plat_irq_save();
    regs_t *frame = p->is_ktask && p->kframe ? p->kframe : &p->saved;

    plat_frame_set_ret(frame, (uint32_t)val);
    spin_lock(&proc_lock);
    p->state = ST_READY;
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);
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

/* READY tasks other than `skip`.  Called with the table lock held. */
static int ready_others(const proc_t *skip)
{
    int n = 0;

    for (int i = 0; i < MAX_PROCS; i++)
        if (&procs[i] != skip && procs[i].state == ST_READY)
            n++;
    return n;
}

static int proc_waiting(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        int st = procs[i].state;
        if (st == ST_WAIT_CHILD || st == ST_WAIT_INPUT || st == ST_WAIT_SLEEP ||
            st == ST_WAIT_PIPE || st == ST_WAIT_THREAD || st == ST_KT_WAIT)
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
 * what normally makes progress here - it wakes sleepers and hands finished
 * console lines to whoever is blocked reading.
 *
 * It cannot be the only source of that progress.  A core that reaches this
 * loop from the MemManage handler that `run prot heap &` produces can go a
 * long time without taking a tick; the console is then never drained, the
 * shell blocked in read() is never woken, and the machine looks dead while
 * every counter still advances.  Measured on the RP2350 with a K/M/P trace:
 * on the rounds that wedged, not even "a line is ready" was ever reached -
 * the input was simply never cooked.
 *
 * So the console owner drains here too.  The drain runs with interrupts
 * masked, because tty_poll() is not reentrant: doing it with them on lets
 * the tick land in the middle of tty_char() and cook the same FIFO twice,
 * which came out as `ps` arriving as "sp" plus a stray "p" and ended in a
 * "no runnable process" panic.  Handing the finished line over happens with
 * interrupts back on - it takes only the process lock. */
static void sched_idle(void)
{
    plat_irq_enable();

    if (cpu_id() == 0) {
        plat_irq_disable();
        tty_poll();
        console_echo_tick();
        plat_irq_enable();
        process_console_kick();
    }
    plat_cpu_idle();
}

/* Hand the hardware the current shape of a task's window.  Called on every
 * switch (protection belongs to whoever is on the CPU) and again when sbrk
 * moves the heap bound.
 *
 * A thread shares its leader's window and has none of its own, so the group
 * leader is what gets programmed.  Thread stacks live inside the group's
 * heap/data range (they are allocated with sbrk by the thread library), so
 * they are covered by the same data region and need no fourth PMP entry. */
#define PROC_GROUP(p) ((p)->leader ? (p)->leader : (p))

static void apply_protection(proc_t *p)
{
    proc_t *g = PROC_GROUP(p);

    plat_task_protect(g->win_base, g->win_base + g->win_size, g->wx_start,
                      g->brk, g->stack_top - g->stack_size, g->stack_top);
}

/* Save the outgoing frame and return the frame to resume.  Callers hold the
 * CPU at the top of a trap, so abandoning the kernel stack below us costs
 * nothing: the next trap starts at the top again. */
static regs_t *sched_switch(regs_t *r)
{
    proc_t *prev = current;
    int first = 1;

    /* Somebody asked for this switch; whoever asked (a wake-up, a syscall)
       does not need to ask again once we are here. */
    need_resched = 0;

    for (;;) {
        proc_t *next;
        int waiting;

        plat_irq_disable();
        spin_lock(&proc_lock);

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

        /* Hand the outgoing task back to the run queue inside the same
         * critical section that picks its successor.  Marking it READY under
         * one lock acquisition and picking under the next leaves a window in
         * which it is READY but still running here - and the other core is
         * scanning for exactly that.  Measured on the RP2350: the shell was
         * run by both cores, each writing the other's saved frame, and died
         * with a register holding an image offset instead of a pointer. */
        if (first) {
            first = 0;
            /* Ownership is `cpu`, not `state`.  A task that blocked in a
             * syscall does its bookkeeping, drops the lock and only then
             * calls us (see block_on_pipe_locked); in that gap the other
             * core may wake it, pick it and set it RUNNING again.  Testing
             * the state alone then says "still mine", and this CPU writes
             * its own trap frame - r0 is still the syscall number - over
             * the result the other core just delivered, and marks the task
             * READY while the other core is executing it.  `cpu` is set to
             * -1 by whoever releases the task and to the new owner by
             * whoever picks it, so comparing it with this CPU answers "do
             * I still own it".  Measured on the RP2350 as a lost syscall
             * return value and as duplicated/missing 128-byte blocks in
             * `piperace 4 64`. */
            if (prev && prev->state == ST_RUNNING && prev->cpu == cpu_id()) {
                prev->saved = *r;
                prev->kframe = r;
                prev->state = ST_READY;
                prev->cpu = -1;

                /* Hand it to the other core if that core has nothing to do.
                 * Without this an idle CPU never sees a READY task at all:
                 * the moment this one publishes its own task it is back
                 * inside the lock re-picking it, and the window is a few
                 * instructions wide.  Measured on the RP2350 with two
                 * workers: both stayed on cpu 0 while cpu 1 spun in its
                 * idle loop, and the workers' throughput was 1:130.
                 *
                 * Only when another runnable task exists - otherwise a
                 * single task would ping-pong between the cores every tick
                 * for no gain. */
                if (cpu_count > 1 && ready_others(prev) > 0) {
                    spin_unlock(&proc_lock);
                    plat_wake_cpu();
                    spin_lock(&proc_lock);
                }
            }
        }

        next = sched_pick();
        waiting = proc_waiting() || other_cpu_busy();
        if (next) {
            next->state = ST_RUNNING;
            next->cpu = cpu_id();
            current = next;
            ktask_running = next->is_ktask ? 1 : 0;
            quantum = QUANTUM;
            spin_unlock(&proc_lock);
            /* A task can move to another core, and on ARM the kernel stack
             * pointer inside its frame points at the core that built it.
             * Re-base it to this one before the frame is restored; a no-op
             * on the ports where the kernel stack is per task (rv32) or
             * lives behind a TSS (x86). */
            plat_set_kernel_stack(KSTACK_TOP);
            if (!next->is_ktask)
                apply_protection(next);
            /* A ktask's live frame is on its own stack; only the bootstrap
             * frame is in proc_t.  The stub in start.S uses the entry sp to
             * leave that frame behind before it touches a C stack. */
            if (next->is_ktask) {
                ktask_entry_sp = next->stack_top;
                return next->kframe ? next->kframe : &next->saved;
            }
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
        sched_idle();       /* interrupts on; poll until the next tick */
    }
}

/* Put the caller to sleep with its frame already saved.  Called with
 * proc_lock held and interrupts off; the caller has already checked the
 * condition it is waiting for under that same lock, so a waker on the other
 * CPU either sees ST_* and hands over the result, or it ran first and the
 * check would have failed.  Splitting these three (check, frame, state) is
 * what made `wait` hang: see docs/OPEN-ITEMS.md §23.2. */
static regs_t *block_locked(regs_t *r, int st, uint32_t irq)
{
    proc_t *p = current;

    p->saved = *r;
    /* Woken by something unexpected?  Better a failed syscall than the
       syscall number handed back as a result. */
    plat_frame_set_ret(&p->saved, (uint32_t)-1);
    p->state = st;
    p->cpu = -1;
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);

    return sched_switch(r);
}

/* Give up the CPU until somebody completes our request.  For conditions
 * whose waker cannot be raced: the tick, or a driver callback that reruns
 * every millisecond.  A one-shot waker (a dying child, a pipe writer) has to
 * call block_locked() inside the critical section that checks the condition,
 * or the wake-up can be spent before this task is sleepable. */
static regs_t *block(regs_t *r, int st)
{
    uint32_t irq = plat_irq_save();

    spin_lock(&proc_lock);
    return block_locked(r, st, irq);
}

static regs_t *done(regs_t *r, int32_t val)
{
    plat_frame_set_ret(r, (uint32_t)val);
    return r;
}

/* Hand queued console input to whoever is blocked reading it.  The tick
 * calls this once per tick; the boards that drive console RX from a real
 * interrupt (the C3's UART0 and USB-Serial/JTAG lines) also call it from
 * the interrupt, right after the drain, so a blocked reader is woken the
 * moment its bytes land instead of at the next tick - a transfer runs at
 * the speed of the wire, not at 100 Hz.
 *
 * Same rule as process_tick: only meaningful on the CPU that owns the
 * console.  Runs in trap context either way, and never on behalf of the
 * current task, so it needs no frame and returns nothing. */
void process_console_kick(void)
{
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
}

void process_tick(regs_t *r)
{
    uint64_t now;

    /* Cheapest moment to notice a blown kernel stack: the tick runs no
       matter what the task is doing, and r points at the trap frame on
       that very stack.  A kernel thread's frame is on its own stack. */
    /* Only a live user task owns the kernel stack.  A ktask frame is on its
     * own stack, and during SYS_KTASK_EXIT the current slot is already free
     * (or NULL) while the frame is still on that stack. */
    if (current && !current->is_ktask && current->state != ST_FREE)
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
        process_console_kick();

        /* Control characters act on the foreground task - whoever is blocked
           reading the console - which is exactly the job the two shells above
           found.  They are handled after the line hand-over so that ^C on an
           empty prompt cannot be swallowed as input. */
        tty_ctrl_deliver();
    }

    now = plat_ticks();
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];
        if ((p->state == ST_WAIT_SLEEP || p->state == ST_KT_WAIT) &&
            p->wake_tick && (int64_t)(now - p->wake_tick) >= 0)
            make_ready(p, 0);
    }

    /* Preempt only a task the timer interrupted in user mode: a tick that
       lands inside a kernel call must let that call finish, because the
       kernel stack below it is not resumable. */
    if (!current || !plat_fault_is_user(r)) {
        return;
    }
    if (quantum > 0)
        quantum--;
    if (quantum == 0)
        need_resched = 1;
}
regs_t *process_resched(regs_t *r)
{
    /* Only a frame that interrupted USER code may consume the request.  A
     * tick (or a console kick's preempt_if_higher) can arm need_resched
     * while the task is in the middle of a kernel call; the IRQ that
     * happens to exit first may then be one that interrupted the KERNEL,
     * and switching here would save that kernel frame over the task's
     * state - the task resumes in user mode from a frame whose r0 is still
     * the syscall number, and whatever the interrupted call had half done
     * is abandoned.  The request simply waits for the next trap that
     * really did interrupt user code. */
    if (need_resched && plat_fault_is_user(r)) {
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

regs_t *process_current_frame(void)
{
    return current ? &current->saved : 0;
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
        /* Leaving the CPU: `cpu` is what the scheduler uses to decide
         * ownership now, so it has to be cleared here as well.  A stopped
         * task that still names this CPU makes sched_switch() believe it
         * owns it and write this trap frame over its saved one. */
        current->cpu = -1;
        return sched_switch(r);
    }
    return r;
}

/* Cheap sanity check of the task layout, run once at boot.  See the comment
 * on PLAT_STACK_BYTES in platform.h for why each condition is there.  This
 * checks the *default* stack; a program that asks for its own (TOS_STACK)
 * is checked against the same rules in exec(), because one bad image must
 * not be able to boot-loosen the board. */
static void layout_check(void)
{
    uint32_t grain = PLAT_APP_GRAIN;
    uint32_t stk = DEFAULT_STACK;
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

void *process_current_task(void)
{
    return current;
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

/* The job a blocked task is waiting for, or 0.  This is the second thing ^C
 * may aim at: `run spin` is CPU-bound and never reads, so it is not a console
 * reader, but it is what the user is looking at and what ^C has to stop.  A
 * pipeline has two such children and the first is taken; a script's shell
 * waiting on its own child could be picked - the console reader keeps
 * priority over both, and the OS has no notion of a job that would settle
 * which shell owns the terminal. */
static int foreground_job(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == ST_WAIT_CHILD && procs[i].wait_pid > 0)
            return procs[i].wait_pid;
    }
    return 0;
}

/* Who ^C stops.  In order: whoever is reading the console; the job a blocked
 * parent waits for; and last the task that outranks the shell on the CPU.
 *
 * The last one is not a catch-all, it is the way back from a runaway
 * promoted job.  `nice 0 spin &` neither reads nor waits, so nothing else
 * ever names it, and at level 0 it keeps the shell from running at all -
 * without this, ^C would have no target and the only exit would be a reset.
 * The listing is by priority, so a machine whose only task is the shell
 * yields 0 and the key expires as it always did.
 *
 * The first task is never a target: it is what reports the kill (and
 * process_signal refuses it anyway). */
int process_ctrl_target(void)
{
    int pid = process_console_reader();

    if (pid > 0 && (!init_proc || pid != init_proc->pid))
        return pid;

    pid = foreground_job();
    if (pid > 0 && (!init_proc || pid != init_proc->pid))
        return pid;

    proc_t *best = NULL;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];

        if (p == init_proc || (p->state != ST_READY && p->state != ST_RUNNING))
            continue;
        if (!best || p->prio < best->prio ||
            (p->prio == best->prio && p->cpu_ms > best->cpu_ms))
            best = p;
    }
    return best ? (int)best->pid : 0;
}

/* Validate a user pointer against the current task's window.  Without an
 * MMU this is a range check, not a security boundary: see PLATFORM-DESIGN. */
int process_user_ok(uint32_t addr, uint32_t len)
{
    if (!current)
        return 0;
    if (len == 0)
        return 1;
    /* A thread has no window of its own: its addresses are the group's. */
    proc_t *g = PROC_GROUP(current);
    if (addr < g->win_base)
        return 0;
    if (addr + len < addr)
        return 0;
    return addr + len <= g->win_base + g->win_size;
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

#if TOS_PMP_POW2_WINDOWS
        /* A NAPOT region must start at a multiple of its own size. */
        cand = align_up(cand, bytes);
        if (cand >= end)
            break;
#endif

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
static void proc_retire_locked(proc_t *p)
{
    p->state = ST_FREE;
    p->pid = 0;
    p->parent = NULL;
    p->leader = NULL;
    p->tgid = 0;
    p->cpu = -1;
    p->win_base = 0;
    p->win_size = 0;
}

static void proc_retire(proc_t *p)
{
    /* An exec that fails after the descriptor inheritance still holds the
     * references fd_clone() took.  Retiring the slot without closing them
     * leaks pipe readers/writers: measured on mps2 as a writer count stuck
     * at 1 with no task holding the end, and a reader parked forever on a
     * pipe nobody will ever close. */
    for (int i = 0; i < MAX_FDS; i++)
        fd_close_in(p, i);
    spin_lock(&proc_lock);
    proc_retire_locked(p);
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

    uint32_t stack_low = p->stack_top - p->stack_size;
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

#if defined(__riscv)
/* ---- kernel threads -------------------------------------------------- *
 *
 * A kernel thread is a proc_t with is_ktask set: no user window, its own
 * stack, M-mode code.  It is scheduled by the same frames as a user task,
 * but two RISC-V details have to be handled explicitly: *
 *   - mret always writes mstatus.MPP=U, so the next trap entry would mistake
 *     a kernel thread for a user task and switch stacks.  ktask_running is
 *     what start.S checks instead; trap_handler forces the saved MPP back to
 *     M so the return stays in machine mode.
 *   - the tick never preempts a kernel thread (process_tick only reschedules
 *     U-mode frames), so a thread gives up the CPU only at an OSI block or
 *     yield.  That is the right shape for the WiFi blob: its tasks park on
 *     queues and are woken by the WiFi interrupt. */
#define SYS_KTASK_SLEEP 60
#define SYS_KTASK_YIELD 61
#define SYS_KTASK_EXIT  62

void ktask_trampoline(void (*fn)(void *), void *arg)
{
    fn(arg);
    ktask_exit();
}

static void ktask_exit(void)
{
    register int a7 __asm__("a7") = SYS_KTASK_EXIT;

    __asm__ volatile("ecall" : : "r"(a7) : "memory");
    for (;;)
        ;
}

void process_ktask_sleep(uint32_t ticks)
{
    register int a0 __asm__("a0") = (int)ticks;
    register int a7 __asm__("a7") = SYS_KTASK_SLEEP;

    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

void process_ktask_yield(void)
{
    register int a7 __asm__("a7") = SYS_KTASK_YIELD;

    __asm__ volatile("ecall" : : "r"(a7) : "memory");
}

int process_ktask_create(const char *name, void (*fn)(void *), void *arg,
                         uint32_t stack_bytes, int prio)
{
    proc_t *p;
    int slot = -1;

    if (!fn)
        return -1;
    if (stack_bytes == 0 || stack_bytes > KTASK_STACK_BYTES)
        stack_bytes = KTASK_STACK_BYTES;

    uint32_t irq = plat_irq_save();
    for (int i = 0; i < KTASK_SLOTS; i++) {
        if (!(ktask_stack_used & (1u << i))) {
            slot = i;
            ktask_stack_used |= 1u << i;
            break;
        }
    }
    if (slot < 0) {
        plat_irq_restore(irq);
        return -1;
    }

    p = proc_alloc();
    if (!p) {
        ktask_stack_used &= ~(1u << slot);
        plat_irq_restore(irq);
        return -1;
    }

    p->is_ktask = 1;
    p->kstack_slot = (uint8_t)slot;
    p->prio = prio;
    p->win_base = 0;
    p->win_size = 0;
    p->wx_start = 0;
    p->stack_top = (uint32_t)&ktask_stacks[slot][KTASK_STACK_BYTES];
    p->stack_size = stack_bytes;
    p->brk = 0;
    p->cpu = -1;
    p->start_ns = plat_uptime_ms();
    set_name(p, name);

    memset(&p->saved, 0, sizeof(p->saved));
    p->saved.mstatus = RV_MSTATUS_MPP_M;   /* MPIE=0: the stub enables it */
    p->saved.mepc = (uint32_t)ktask_start_stub;
    p->saved.sp = 0;                        /* unused; the stub sets sp */
    p->saved.ra = (uint32_t)ktask_exit;
    p->saved.a0 = (uint32_t)fn;
    p->saved.a1 = (uint32_t)arg;
    p->kframe = &p->saved;

    spin_lock(&proc_lock);
    p->pid = next_pid++;
    p->state = ST_READY;
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);
    return p->pid;
}

regs_t *process_ktask_syscall(regs_t *r)
{
    proc_t *p = current;

    if (!p || !p->is_ktask) {
        plat_frame_set_ret(r, (uint32_t)-1);
        return r;
    }

    /* The trap entry came through the ktask path, so the frame is on the
     * ktask stack.  Force MPP back to M: mret must return to machine mode,
     * and mret will clear MPP again for the next entry (which uses the
     * flag, not MPP). */
    r->mstatus = (r->mstatus & ~RV_MSTATUS_MPP_MASK) | RV_MSTATUS_MPP_M;

    switch (r->a7) {
    case SYS_KTASK_SLEEP: {
        uint32_t ticks = r->a0 ? (uint32_t)r->a0 : 1u;

        p->saved = *r;
        p->kframe = r;
        plat_frame_set_ret(r, 0);
        p->wake_tick = plat_ticks() + ticks;
        p->state = ST_KT_WAIT;
        p->cpu = -1;
        return sched_switch(r);
    }

    case SYS_KTASK_YIELD:
        p->saved = *r;
        p->kframe = r;
        plat_frame_set_ret(r, 0);
        p->state = ST_READY;
        p->cpu = -1;
        return sched_switch(r);

    case SYS_KTASK_EXIT: {
        uint32_t irq = plat_irq_save();

        ktask_stack_used &= ~(1u << p->kstack_slot);
        p->is_ktask = 0;
        p->kstack_slot = 0;
        spin_lock(&proc_lock);
        proc_retire_locked(p);
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        current = NULL;
        return sched_switch(r);
    }

    default:
        plat_frame_set_ret(r, (uint32_t)-1);
        return r;
    }
}
#endif /* __riscv */

/* A TOS_STACK() request (or the same value arriving in an envelope hint)
   has to be one the protection unit can describe and the window grain can
   hold.  Both paths use this so a hint cannot bypass the check the ELF note
   has always had. */
static int stack_request_ok(uint32_t v)
{
    return (v & (v - 1u)) == 0 && v >= PLAT_PAGE_SIZE &&
           PLAT_APP_GRAIN % v == 0;
}

/* ---- streaming exec ----------------------------------------------------
 *
 * Every user program is a hinted LZ4TOS envelope.  Its 28-byte header says
 * how big the raw ELF is and how big a window the image needs, so exec()
 * can reserve the window first and decompress straight into it.  The kernel
 * heap never holds a copy of the image - the only allocation is the
 * littlefs file cache behind ramdisk_open().  Both helpers own their handle,
 * so every failure path is clean.  A file without the hint is refused
 * loudly; there is no 16-byte-header fallback to keep alive. */

/* Returns the LZ4 block offset, or 0 when this is not a loadable program. */
static uint32_t envelope_probe(const char *path, uint32_t *raw_size,
                               lz4_window_hint_t *hint)
{
    uint8_t prefix[LZ4_WRAP_HEADER_V1];
    uint32_t got = 0;

    *raw_size = 0;
    memset(hint, 0, sizeof(*hint));

    int h = ramdisk_open(path);
    if (h < 0)
        return 0;
    while (got < sizeof(prefix)) {
        int n = ramdisk_read(h, prefix + got,
                             (uint32_t)sizeof(prefix) - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    uint32_t fsize = ramdisk_size(h);
    ramdisk_close(h);

    uint32_t rs = 0;
    uint32_t hdr = lz4_wrap_info_prefix(prefix, got, fsize, &rs, hint);
    if (!hdr)
        return 0;
    *raw_size = rs;
    return hdr;
}

static int exec_stream_read(void *ctx, uint8_t *buf, uint32_t cap)
{
    int h = *(int *)ctx;
    uint32_t got = 0;
    while (got < cap) {
        int n = ramdisk_read(h, buf + got, cap - got);
        if (n < 0)
            return -1;
        if (n == 0)
            break;                 /* EOF */
        got += (uint32_t)n;
    }
    return (int)got;
}

static int stream_load(const char *path, uint32_t hdr, uint8_t *dst,
                       uint32_t raw_size)
{
    int h = ramdisk_open(path);
    if (h < 0)
        return -1;

    int rc = -1;
    if (ramdisk_seek(h, hdr) == 0) {
        lz4_stream_t s;
        memset(&s, 0, sizeof(s));
        s.read = exec_stream_read;
        s.ctx = &h;
        int n = lz4_stream_decompress(&s, dst, raw_size);
        if (n >= 0 && (uint32_t)n == raw_size)
            rc = 0;
    }
    ramdisk_close(h);
    return rc;
}

static proc_t *proc_create_claim(const char *path, char *const argv[], int argc,
                                 int claim)
{
    uint32_t raw_size = 0;
    lz4_window_hint_t hint = {0, 0, 0};
    uint32_t hdr = envelope_probe(path, &raw_size, &hint);
    if (!hdr) {
        kprintf("exec: '%s' not found or not a loadable program\n", path);
        return NULL;
    }

    /* Claim the table slot first: win_alloc() records the window in this
       very entry, and proc_retire() gives both back on any failure below.
       Nothing here owns a heap buffer - the image lives in the window. */
    proc_t *p = proc_alloc();
    if (!p) {
        kprintf("exec: process table full\n");
        return NULL;
    }

    /* Everything the window needs is on the envelope.  No board value is
       stored in it: the default stack, guard and grain are ours, so the
       same image still loads on every target.  The ELF is checked against
       these numbers once it has been decompressed. */
    uint32_t stk = hint.stack_ask ? hint.stack_ask : DEFAULT_STACK;
    if (hint.stack_ask && !stack_request_ok(hint.stack_ask)) {
        kprintf("exec: '%s' carries a bad stack hint (%u bytes)\n",
                path, hint.stack_ask);
        proc_retire(p);
        return NULL;
    }
    /* The decompressor writes the whole raw ELF linearly into the window
       before the segments are slid into place, so the raw file size - not
       just the page-rounded memory span - is a line item.  For our images
       span wins, but a 64 KiB program on a small board is exactly the case
       where it might not. */
    uint32_t img = hint.span > raw_size ? hint.span : raw_size;
    uint32_t win_need = img;
    uint32_t app_total = PLAT_APP_MAX - PLAT_APP_BASE;
    if (add_u32_ok(win_need, stk, &win_need) < 0 ||
        add_u32_ok(win_need, hint.heap_ask, &win_need) < 0 ||
        add_u32_ok(win_need, HEAP_GUARD, &win_need) < 0) {
        kprintf("exec: '%s' window size overflows\n", path);
        proc_retire(p);
        return NULL;
    }
    if (win_need > app_total) {
        kprintf("exec: '%s' needs %u KiB, the whole user area is %u KiB\n",
                path, win_need / 1024u, app_total / 1024u);
        proc_retire(p);
        return NULL;
    }
    uint32_t win;
    if (align_up_ok(win_need, PLAT_APP_GRAIN, &win) < 0) {
        kprintf("exec: '%s' window alignment overflows\n", path);
        proc_retire(p);
        return NULL;
    }
    if (win < PLAT_APP_GRAIN)
        win = PLAT_APP_GRAIN;
#if TOS_PMP_POW2_WINDOWS
    win = win_pow2(win);
    if (win > app_total) {
        kprintf("exec: '%s' needs a %u KiB window, the largest power-of-two "
                "window is %u KiB\n", path, win / 1024u, app_total / 1024u);
        proc_retire(p);
        return NULL;
    }
#endif
    if (win_alloc(p, win) != 0) {
        kprintf("exec: '%s' needs a %u KiB window, the largest free block is %u KiB\n",
                path, win / 1024u, win_largest_free() / 1024u);
        proc_retire(p);
        return NULL;
    }
    uint32_t base = p->win_base;

    /* The decompressor is given raw_size, so prove the destination really
     * has that many bytes before letting it write. */
    if (raw_size > win) {
        kprintf("exec: '%s' image is larger than its window\n", path);
        proc_retire(p);
        return NULL;
    }

    /* Decode the envelope straight into the reserved window. */
    if (stream_load(path, hdr, (uint8_t *)base, raw_size) != 0) {
        kprintf("exec: '%s' is corrupt (bad LZ4 block)\n", path);
        proc_retire(p);
        return NULL;
    }
    uint8_t *data = (uint8_t *)base;
    uint32_t size = raw_size;

    /* Re-read what the ELF says now that it is here, and refuse a stale or
       tampered hint before the loader can write past an undersized window. */
    uint32_t link_lo = 0, link_hi = 0, link_entry = 0, link_wx = 0;
    if (elf_probe(data, size, &link_lo, &link_hi, &link_entry, &link_wx) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        proc_retire(p);
        return NULL;
    }
    uint32_t asked_stack = 0, asked_heap = 0;
    if (elf_tos_note(data, size, &asked_stack, &asked_heap) == 0) {
        if (asked_stack != 0 && !stack_request_ok(asked_stack)) {
            kprintf("exec: '%s' asks for a %u-byte stack; this board "
                    "needs a power of two from %u to %u KiB\n",
                    path, asked_stack, PLAT_PAGE_SIZE / 1024u,
                    PLAT_APP_GRAIN / 1024u);
            proc_retire(p);
            return NULL;
        }
    }
    if (link_hi < link_lo) {
        kprintf("exec: '%s' has an inverted memory span\n", path);
        proc_retire(p);
        return NULL;
    }
    uint32_t span, win_elf;
    if (align_up_ok(link_hi - link_lo, PLAT_PAGE_SIZE, &span) < 0) {
        kprintf("exec: '%s' span overflows\n", path);
        proc_retire(p);
        return NULL;
    }
    win_elf = span;
    if (add_u32_ok(win_elf, stk, &win_elf) < 0 ||
        add_u32_ok(win_elf, asked_heap, &win_elf) < 0 ||
        add_u32_ok(win_elf, HEAP_GUARD, &win_elf) < 0 ||
        align_up_ok(win_elf, PLAT_APP_GRAIN, &win_elf) < 0) {
        kprintf("exec: '%s' ELF window overflows\n", path);
        proc_retire(p);
        return NULL;
    }
    if (win_elf < PLAT_APP_GRAIN)
        win_elf = PLAT_APP_GRAIN;
    if (span != hint.span || asked_stack != hint.stack_ask ||
        asked_heap != hint.heap_ask || win_elf > win || raw_size > win) {
        kprintf("exec: '%s' window hint disagrees with its ELF\n", path);
        proc_retire(p);
        return NULL;
    }

    /* The loader slides the segments into place, clears .bss, applies
       relocations and checks every write against the window. */
    elf_set_window(base, base + win);
    uint32_t entry = 0;
    if (elf_load(data, size, win, base, &entry) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        elf_set_window(0, 0);
        proc_retire(p);
        return NULL;
    }
    elf_set_window(0, 0);

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
            p->fds[i].type = FD_CONSOLE;

    /* Children inherit: a task cannot spawn something more important than
       itself, which is what keeps a program from locking the console out.
       The first task starts at NORMAL and not at the top, so that level 0
       means something: it is where `nice 0` puts a job that has to run on
       time, and an inherited level would leave that knob with nothing above
       the shell to reach. */
    p->prio = current ? current->prio : PRIO_NORMAL;
    p->start_ns = plat_uptime_ms();
    /* win_base/win_size were recorded by win_alloc() - under the lock, so
       nobody else can be handed this range while the image loads. */
    p->wx_start = base + (link_wx - link_lo);
    /* The stack occupies the very top of the window, so that its region is
       naturally aligned; the initial frame starts a little below.  Its size
       is this task's own now, and every later use - protection, the argv
       block, the poison fill, the high-water scan - goes through
       stack_size so the four cannot drift apart. */
    p->stack_top = base + win;
    p->stack_size = stk;
    p->heap_base = base + span;
    p->heap_end = p->stack_top - stk - HEAP_GUARD;

    /* Fill the stack with a poison byte before anything uses it, so the
       deepest address the program ever wrote to can be recovered later: the
       first byte from the bottom that is no longer 0xCC is the high-water
       mark (see stack_used()).  Done here, before build_initial_stack()
       writes argv on top, so the arguments count as usage - they are memory
       the program can see. */
    memset((void *)(p->stack_top - stk), STACK_POISON, stk);
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
       has to be untouched by ordinary stack use, so the initial frame plus
       that margin has to fit inside the stack or the task must be refused
       here instead of faulting on its first interrupt.
       
       This used to also refuse any program whose image was bigger than the
       stack, on the theory that a big program probably needs a big stack.
       That was a guess, and now that a program states its own stack the
       guess is wrong in the one direction that matters: `dd` is a 12.8 KiB
       image with a 0.5 KiB stack footprint (ps's stk column), and refusing
       it for having too little stack is refusing it for being small.  What
       is left is the only hard requirement - the initial frame fits - and
       everything else is the program's own business, measured with stk and
       declared with TOS_STACK(). */
    uint32_t need = (argv_block_start(p, argc) - sp) + 256u;
    if (sp - need < p->stack_top - stk) {
        kprintf("exec: '%s' needs %u bytes of stack for its arguments and the "
                "initial frame, the stack is %u\n", path, need, stk);
        proc_retire(p);
        return NULL;
    }

    /* The frame this task will resume on.  Nothing else is needed to start
       it: the trap epilogue restores exactly this. */
    plat_prepare_user_frame(&p->saved, p->entry, p->user_esp);

    /* Publish it.  The state store is the last write and it happens under
       the table lock, so a scheduler on another CPU either sees a free slot
       or a finished task, never a half-built one.
       
       `claim` is for the one caller that is also the task's first runner:
       the initial shell.  It is published as already RUNNING instead of
       READY, with `current` set, so a secondary CPU cannot take it in the
       window between the publish and the caller's own bookkeeping.  That
       window is not theoretical - with core 1 up, `starting /bin/msh` is
       followed by a boot that dies in the shell roughly one time in three,
       because both cores end up running it on different stacks. */
    spin_lock(&proc_lock);
    p->state = claim ? ST_RUNNING : ST_READY;
    if (claim) {
        p->cpu = cpu_id();
        current = p;
        quantum = QUANTUM;
    }
    spin_unlock(&proc_lock);
    return p;
}

static proc_t *proc_create(const char *path, char *const argv[], int argc)
{
    return proc_create_claim(path, argv, argc, 0);
}

int process_exec(regs_t *r, const char *path, char *const argv[], int argc)
{
    if (!current)
        return -1;

    /* exec() replaces the address space, so it cannot be called by a thread
     * that shares one with its siblings: the new image would pull the window
     * out from under them.  POSIX solves this by killing the other threads;
     * for now the call simply fails. */
    if (current->leader)
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
 * way to keep that from happening in ~40 bytes is to not allow it.
 *
 * The first task is the exception, and the whole of it: the shell may place
 * a child above itself, which is how `nice 0 samp` gets a 1 ms job the CPU
 * ahead of everything else.  It is the user's own keystroke, the task that
 * asked is not the one that gets promoted, and ^C still reaches the job
 * (process_ctrl_target falls back to the task a blocked parent waits for,
 * and then to whatever outranks the shell), so a promoted job cannot outlive
 * the terminal that started it.  Nothing a normal program does gets this - a
 * child it promotes is promoted by it, not by the shell. */
int process_setprio(int pid, int prio)
{
    proc_t *t;

    if (prio < 0 || prio >= PRIO_LEVELS)
        return -1;
    if (current && prio < current->prio && current != init_proc)
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
    /* A thread owns no window, no children and (for now) only console fds.
     * The group leader does the real teardown. */
    if (p->leader)
        return;

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

/* Remember a child's exit code for a wait() that has not happened yet.
 * Called with the table lock held. */
static void lag_push(proc_t *p, int pid, int code)
{
    if (p->lag_n >= LAG_MAX)
        return;                 /* the oldest is lost; see proc_t.lag_pid */
    p->lag_pid[p->lag_n] = pid;
    p->lag_code[p->lag_n] = code;
    p->lag_n++;
}

/* Take the code of child `pid` (any child when pid < 0).  Returns 0 when
 * that child is not in the list.  Called with the table lock held. */
static int lag_take(proc_t *p, int pid, int *code)
{
    for (int i = 0; i < p->lag_n; i++) {
        if (pid >= 0 && p->lag_pid[i] != pid)
            continue;
        *code = p->lag_code[i];
        p->lag_n--;             /* order does not matter, so fill the hole */
        p->lag_pid[i] = p->lag_pid[p->lag_n];
        p->lag_code[i] = p->lag_code[p->lag_n];
        return 1;
    }
    return 0;
}

/* Hand a dead task's exit code to whoever waits for it: an actual waiter
 * gets it in its saved frame, otherwise it is remembered for the next
 * wait().  Children of the dead task go to its parent, or to init.
 *
 * Called with proc_lock held.  Returns the parent if it was made runnable;
 * the caller has to unlock before preempt_if_higher(), which touches
 * per-CPU state.  This runs in the same critical section as the retire (see
 * retire_and_reap) so that a parent scanning for a live child can never see
 * it in the gap between "the code is recorded" and "the slot is free". */
static proc_t *reap_into_parent_locked(proc_t *p, int code)
{
    proc_t *parent = p->parent;
    proc_t *woke = NULL;

    if (parent) {
        if (parent->state == ST_WAIT_CHILD &&
            (parent->wait_pid == p->pid || parent->wait_pid < 0)) {
            /* Same as make_ready(), but the lock is already held and this
             * must not release it: the frame write and the state store stay
             * in the sender's critical section. */
            plat_frame_set_ret(&parent->saved, (uint32_t)code);
            parent->state = ST_READY;
            woke = parent;
        } else {
            lag_push(parent, p->pid, code);
        }
    }

    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != ST_FREE && procs[i].parent == p)
            procs[i].parent = parent ? parent : init_proc;
    }
    return woke;
}

/* The last act of a task: record its code and give the slot back as one
 * step.  Two separate steps left a window in which the slot was neither
 * waiting-for-us nor free, and a parent that scanned there would block
 * forever on a result that had already been recorded. */
static void retire_and_reap(proc_t *p, int code)
{
    uint32_t irq = plat_irq_save();
    proc_t *woke;

    spin_lock(&proc_lock);
#ifdef TOS_KTHREADS
    /* A process leader takes its threads with it: they share the window that
     * is about to disappear. */
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].leader == p)
            proc_retire_locked(&procs[i]);
#endif
    woke = reap_into_parent_locked(p, code);
    proc_retire_locked(p);
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);

    if (woke)
        preempt_if_higher(woke);
}

/* Retire a task that is not the caller: the caller keeps the CPU, the
 * victim's frame is never resumed again.  Used by SIGKILL; the init task
 * cannot be retired this way (it is what shuts the machine down). */
static void task_kill(proc_t *p, int code)
{
    task_release(p);
    retire_and_reap(p, code);
}

regs_t *process_exit(regs_t *r, int code)
{
    proc_t *p = current;
    if (!p)
        panic("no current process");

    task_release(p);

    /* Nothing can read messages addressed to a task that is leaving; give
     * their slots back now instead of waiting for the pool to fill up. */
    mailbox_drop(p->pid);

    int is_init = (p == init_proc);
    if (is_init) {
        /* Raise the flag *before* the slot goes back to the table: a CPU
         * that sees an empty run queue must be able to see it, and the only
         * way to guarantee that without a shared clock is to make the flag
         * visible first.  Folding the reap in lets it share the lock, and
         * keeps the flag ahead of the slot as before. */
        uint32_t irq = plat_irq_save();
        proc_t *woke;

        spin_lock(&proc_lock);
        sys_down = 1;
#ifdef TOS_KTHREADS
        for (int i = 0; i < MAX_PROCS; i++)
            if (procs[i].leader == p)
                proc_retire_locked(&procs[i]);
#endif
        woke = reap_into_parent_locked(p, code);
        proc_retire_locked(p);
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);

        if (woke)
            preempt_if_higher(woke);
    } else {
        retire_and_reap(p, code);
    }

    current = NULL;

    if (is_init) {
        kprintf("\ninit process exited with code %d - shutting down\n", code);
        plat_shutdown();
    }

    regs_t *nr = sched_switch(r);
    return nr;
}

regs_t *process_wait(regs_t *r, int pid)
{
    proc_t *p = current;
    uint32_t irq = plat_irq_save();
    int code;

    /* The scan and the sleep are one critical section.  A child exiting on
     * the other CPU does reap + retire under this same lock, so there are
     * only two orders and both are correct:
     *
     *   it exits first  -> the slot is already ST_FREE, the loop skips it,
     *                      and the code it recorded is taken below;
     *   we scan first   -> it finds parent->state == ST_WAIT_CHILD and
     *                      hands the code straight into our saved frame.
     *
     * The old code scanned outside the lock and slept afterwards, which left
     * a third order: recorded, then we sleep, then nobody ever wakes us. */
    spin_lock(&proc_lock);
    /* A child that has already gone is in the lag list, and taking it first
     * is what keeps the codes straight: several children can exit between
     * two wait() calls, and the one asked about is not necessarily the last
     * to go. */
    if (lag_take(p, pid, &code)) {
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        return done(r, code);
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *c = &procs[i];
        if (c->state == ST_FREE || c->parent != p)
            continue;
        if (pid >= 0 && c->pid != pid)
            continue;
        p->wait_pid = pid;
        return block_locked(r, ST_WAIT_CHILD, irq);
    }

    /* No live child by that pid, and no remembered code either: it never
       existed (or it is one of the exits that did not fit in lag_pid). */
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);
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

    /* "Same as current" only means "on this CPU" while it is actually
     * running.  sched_switch() idles without clearing `current`, so a task
     * asleep with the CPU parked beside it is still `current` - and taking
     * the deferred branch there put the SIGKILL in kill_pending, to be acted
     * on at a trap exit the sleeper would not reach for another three
     * seconds.  Measured: ^C on a foreground `sleep 3000` did nothing until
     * the nap was over.  A blocked task has no live frame to protect; block()
     * copied its registers into ->saved before switching away. */
    if (victim == current && victim->state == ST_RUNNING) {
        if (sig == SIGKILL)
            victim->kill_pending = 1;
        else if (sig == SIGSTOP)
            victim->stop_pending = 1;
        return 0;
    }

    if (sig == SIGKILL) {
        int was_current = (victim == current);

        kprintf("\n[pid %d] killed\n", pid);
        /* The caller keeps the CPU; the victim simply never runs again.
           Its exit code reaches a waiting parent through lag_pid. */
        task_kill(victim, 137);
        /* The CPU was only parked next to it, not inside it: stop pointing
           at a slot that is now free, the way process_exit() does. */
        if (was_current)
            current = NULL;
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

/* ---- kernel threads (TOS_KTHREADS) ---------------------------------- */

#ifdef TOS_KTHREADS

#if !defined(__riscv)
#error "TOS_KTHREADS is implemented for RV32 only: the thread entry frame uses a0 and the RV32 syscall ABI.  See docs/REAL-THREADS.md."
#endif

/* User pointers are checked against the *group* window: a thread slot has no
 * window of its own, so process_user_ok(current, ...) would reject every
 * address the thread legitimately owns. */
static int group_user_ok(proc_t *g, uint32_t addr, uint32_t len)
{
    uint32_t end;

    if (len == 0)
        return 1;
    end = addr + len;
    if (end < addr)
        return 0;
    return addr >= g->win_base && end <= g->win_base + g->win_size;
}

/* Called with proc_lock held.  Hands `code` to a sibling blocked in join()
 * and makes it runnable; returns it so the caller can preempt outside the
 * lock. */
static proc_t *wake_joiner_locked(proc_t *tid_task, int code)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *q = &procs[i];

        if (q->state != ST_WAIT_THREAD || q->wait_pid != tid_task->pid)
            continue;
        if (q->wbuf && group_user_ok(PROC_GROUP(q), q->wbuf, sizeof(int)))
            *(int *)q->wbuf = code;
        plat_frame_set_ret(&q->saved, 0);
        q->state = ST_READY;
        q->cpu = -1;
        return q;
    }
    return NULL;
}

int process_thread_create(uint32_t entry, uint32_t arg, uint32_t ustack)
{
    proc_t *leader = current ? PROC_GROUP(current) : NULL;
    proc_t *p;
    uint32_t irq;
    int pid;

    if (!leader || leader->is_ktask || leader->win_size == 0)
        return -1;
    if (entry == 0 || (ustack & 3u))
        return -1;
    if (ustack <= leader->win_base ||
        ustack > leader->win_base + leader->win_size)
        return -1;

    p = proc_alloc();
    if (!p)
        return -1;

    p->leader = leader;
    p->tgid = leader->tgid;
    p->prio = current->prio;
    p->parent = leader;
    p->stack_top = ustack;
    p->stack_size = 0;              /* protection comes from the group */
    p->heap_base = leader->heap_base;
    p->heap_end = leader->heap_end;
    p->brk = leader->brk;
    memcpy(p->cwd, leader->cwd, sizeof(p->cwd));
    p->start_ns = plat_now_ns();

    /* The experiment's threads get the console and nothing else.  Copying a
     * file/pipe descriptor would double-close the underlying handle when the
     * thread exits; sharing the group fd table is the next step. */
    for (int i = 0; i < MAX_FDS; i++)
        if (leader->fds[i].type == FD_CONSOLE)
            p->fds[i] = leader->fds[i];

    plat_prepare_user_frame(&p->saved, entry, ustack);
    p->saved.a0 = arg;

    irq = plat_irq_save();
    spin_lock(&proc_lock);
    pid = next_pid++;
    p->pid = pid;
    p->name[0] = 't';
    p->name[1] = 'h';
    p->name[2] = 'r';
    p->name[3] = (char)('0' + (pid % 10));
    p->name[4] = '\0';
    p->state = ST_READY;
    p->cpu = -1;
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);

    return pid;
}

regs_t *process_thread_exit(regs_t *r, int code)
{
    proc_t *p = current;
    proc_t *leader;
    proc_t *woke = NULL;
    uint32_t irq;

    if (!p || !p->leader)
        return process_exit(r, code);

    leader = p->leader;
    irq = plat_irq_save();
    spin_lock(&proc_lock);
    /* join(tid) may already have been called: the code has to survive the
     * slot, so it goes into the group's lag list like a child's exit code. */
    lag_push(leader, p->pid, code);
    woke = wake_joiner_locked(p, code);
    proc_retire_locked(p);
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);

    if (woke)
        preempt_if_higher(woke);
    return sched_switch(r);
}

regs_t *process_thread_join(regs_t *r, int tid, uint32_t codeptr)
{
    proc_t *me = current;
    proc_t *g = me ? PROC_GROUP(me) : NULL;
    proc_t *t = NULL;
    uint32_t irq;
    int code;

    if (!g)
        return done(r, -1);

    irq = plat_irq_save();
    spin_lock(&proc_lock);

    if (lag_take(g, tid, &code)) {
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        if (codeptr) {
            if (!group_user_ok(g, codeptr, sizeof(int)))
                return done(r, -1);
            *(int *)codeptr = code;
        }
        return done(r, 0);
    }

    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != ST_FREE && procs[i].pid == tid) {
            t = &procs[i];
            break;
        }
    if (!t || t->leader != g || t == me) {
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        return done(r, -1);
    }
    if (codeptr && !group_user_ok(g, codeptr, sizeof(int))) {
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        return done(r, -1);
    }

    me->wait_pid = tid;
    me->wbuf = codeptr;
    return block_locked(r, ST_WAIT_THREAD, irq);
}

#endif /* TOS_KTHREADS */

/* ---- misc ------------------------------------------------------------ */

uint32_t process_sbrk(uint32_t bytes)
{
    if (!current)
        return 0;

    uint32_t old = PROC_GROUP(current)->brk;
    if (bytes == 0)
        return old;

    uint32_t newbrk = old + bytes;
    if (newbrk < PROC_GROUP(current)->heap_base ||
        newbrk > PROC_GROUP(current)->heap_end)
        return 0;

    /* Threads share the leader's heap: an allocation in one has to move the
     * break for all of them, or two threads hand out the same bytes. */
    PROC_GROUP(current)->brk = newbrk;
    /* The heap bound is part of the protection: without this the new pages
       would stay unreachable for the task that just asked for them. */
    apply_protection(current);
    return old;
}

/* The stack is a soft limit.  Called from a port's user-fault path when a
 * load or store left the stack: if the address it wanted is still inside the
 * task's own window, the stack is doubled into the heap's unused tail and
 * the instruction is retried.  Nothing is reserved for this ahead of time -
 * a window is sized for the stack the program asked for, and the slack
 * between the break and the guard belongs to the heap until the stack needs
 * it, which is the whole point: the two share one region.
 *
 * Two conditions are not negotiable:
 *
 *   - the new low has to be encodable.  PMP NAPOT (rv32) and MPU regions
 *     describe a power of two aligned to itself, so the sizes reachable from
 *     here are 2x, and only while stack_top is a multiple of it.  That is
 *     what keeps the rule count at four and lets an 8 KiB task and a 16 KiB
 *     task share the machine.
 *   - the heap must not be in the way: brk + guard has to stay below the new
 *     low, so the hole that turns the *next* overflow into a fault instead of
 *     a silent overwrite keeps its board size.
 *
 * Returning 1 means the faulting instruction can be re-executed unchanged:
 * it was refused before it wrote anything.  0 leaves the decision to the
 * caller, which kills the task exactly as it always did. */
int process_stack_grow(uint32_t addr, uint32_t sp)
{
    proc_t *p = current;
    uint32_t old, want, low;

    if (!p || p->win_size == 0 || p->stack_size == 0)
        return 0;

    old = p->stack_size;
    want = old << 1;
    if (want < old)
        return 0;

    /* The stack pointer itself has to have left the region.  A stray pointer
     * into the hole while sp is still inside the stack is a bug in the
     * program, not a stack that ran out, and handing it 8 KiB more would
     * simply hide the bug - and shrink the heap of a program that did not
     * ask for it. */
    if (sp >= p->stack_top - old)
        return 0;

    if ((p->stack_top % want) != 0)
        return 0;                            /* the port cannot describe it */

    low = p->stack_top - want;
    if (addr < low)
        return 0;                            /* deeper than one step reaches */

    if (p->brk + HEAP_GUARD > low)
        return 0;                            /* the heap is in the way */

    /* Poison the new strip before it becomes writable: stack_used() scans up
     * from the bottom and would otherwise report the heap's leftovers as
     * stack the program had used. */
    memset((void *)low, STACK_POISON, p->stack_top - old - low);

    p->stack_size = want;
    p->heap_end = low - HEAP_GUARD;
    apply_protection(p);

    kprintf("\n[pid %d] stack grown to %u KiB, heap ceiling %u KiB\n",
            p->pid, want / 1024u, (p->heap_end - p->heap_base) / 1024u);
    return 1;
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
    case ST_KT_WAIT:    return "ktwait";
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

    if (p->win_size == 0 || p->stack_top == 0 || p->stack_size == 0)
        return 0;
    low = (const uint8_t *)(p->stack_top - p->stack_size);
    while (i < p->stack_size && low[i] == STACK_POISON)
        i++;
    return p->stack_size - i;
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
 * look tiny: one tick is 1 ms of CPU but the task runs for a whole slice
 * including the timer's own work.  cpums is the time spent on the CPU, cpu%
 * its share of the machine since the task started.  The `c` column is the CPU
 * running it right now ("-" when it is not on one): on a two-hart build it is
 * what makes two tasks both saying "run" readable, and it is how a program
 * checks that it migrated.  `stk/sz` is the stack high-water mark in KiB with
 * one decimal over the stack the task has *now*: the board default, a
 * TOS_STACK() request, or the 2x a fault handed it when its window had heap
 * slack to spare (process_stack_grow).  The numerator is the number that says
 * whether the size is right; the denominator says which size is being
 * judged. */
uint32_t process_ps_text(char *buf, uint32_t cap)
{
    uint32_t off;
    uint64_t now = plat_uptime_ms();
    int n;

    if (!buf || cap < 64)
        return 0;

    n = ksnprintf(buf, cap, " pid ppid prio state  c cpu%% cpums wink stk/sz  name\n");
    if (n < 0)
        return 0;
    off = (uint32_t)n;

    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];
        char cpubuf[4];
        uint32_t age, busy, pct10, stk10;

        if (p->state == ST_FREE || p->is_ktask)
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
        /* The stack size is whatever this task holds right now, so it is
           printed as is: the formatter has no left-justified width, and a
           column that shifts by one character when a stack doubles is a
           small price for not printing "%-2u" on the console. */
        n = ksnprintf(buf + off, cap - off,
                      "%4d %4d %4d %s %2s %4u.%u %5u %4u %4u.%u/%u  %s\n", p->pid,
                      p->parent ? p->parent->pid : 0, p->prio,
                      state_name(p->state), cpubuf, pct10 / 10u, pct10 % 10u,
                      busy / 1000u, p->win_size / 1024u,
                      stk10 / 10u, stk10 % 10u, p->stack_size / 1024u, p->name);
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

/* Hand a pipe to one sleeping task: whoever moved data completes the transfer
 * on the waiter's behalf, so nothing has to be replayed when it resumes (the
 * same trick the console line hand-over uses).
 *
 * Called with proc_lock held and interrupts off, by the reader or writer that
 * just moved data and is still inside its own critical section - so the task
 * it hands over to cannot have parked itself in between (see the FD_PIPE_R
 * and FD_PIPE_W cases).  Returns the task made runnable, or NULL.
 *
 * One transfer per call, and the caller loops: one event can satisfy more
 * than one waiter (two readers parked on a pipe that then hits EOF both have
 * to see the 0, or the second one waits for a wake-up nobody will send). */
static proc_t *pipe_deliver_locked(struct pipe *p, int dir)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *w = &procs[i];
        int n;

        if (w->state != ST_WAIT_PIPE || w->wpipe != p || w->wdir != dir)
            continue;

        if (dir == 0) {
            n = pipe_read(p, (void *)w->wbuf, w->wlen);
            if (n == PIPE_EAGAIN)
                return NULL;        /* still empty, keep waiting */
        } else {
            n = pipe_write(p, (const void *)w->wbuf, w->wlen);
            if (n == PIPE_EAGAIN)
                return NULL;        /* still full, keep waiting */
            /* Every reader closed while we waited: fail the write instead
             * of parking it forever. */
            if (n == PIPE_EPIPE)
                n = -1;
        }
        w->wpipe = NULL;
        plat_frame_set_ret(&w->saved, (uint32_t)n);
        w->state = ST_READY;
        return w;                   /* n == 0 is EOF and is delivered too */
    }
    return NULL;
}

/* Every waiter that can be satisfied right now.  Returns the one with the
 * best priority, for the caller's post-unlock preempt_if_higher(). */
static proc_t *pipe_wake_locked(struct pipe *p, int dir)
{
    proc_t *best = NULL;
    proc_t *w;

    while ((w = pipe_deliver_locked(p, dir)) != NULL)
        if (!best || w->prio < best->prio)
            best = w;
    return best;
}

static regs_t *block_on_pipe_locked(regs_t *r, struct pipe *p, int dir,
                                    uint32_t buf, uint32_t len, uint32_t irq)
{
    proc_t *me = current;

    me->saved = *r;
    plat_frame_set_ret(&me->saved, (uint32_t)-1);
    me->wpipe = p;
    me->wdir  = dir;
    me->wbuf  = buf;
    me->wlen  = len;
    me->state = ST_WAIT_PIPE;
    me->cpu   = -1;
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);

    return sched_switch(r);
}

/* One read on fd 0 with the console attached: a line at a time in cooked
 * mode, a byte at a time in raw mode (an editor draws its own screen and
 * cannot wait for a newline). */
static regs_t *console_read(regs_t *r, uint32_t ptr, uint32_t len)
{
    if (tty_is_raw()) {
        /* The cap bounds one handover, not the transfer: with the 4 KiB
         * raw queue a bulk reader (user/rx.c) is woken per receive
         * interrupt and drains what is there. */
        uint32_t room = len < 1024 ? len : 1024;
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
        /* The ring is guarded by the table lock, not by one of its own: it
         * is a leaf object whose whole lifetime (including the free in
         * fd_close_in) is decided by tasks, and a second lock here would
         * only add an order to get wrong.  Taking it also closes the gap
         * between "the pipe is empty" and "I am asleep": a writer on the
         * other CPU runs pipe_write and pipe_wake_locked inside this same
         * lock, so it either sees ST_WAIT_PIPE and delivers, or it ran
         * first and the read above would have returned the data. */
        uint32_t irq = plat_irq_save();
        proc_t *woke;
        int n;

        spin_lock(&proc_lock);
        n = pipe_read(f->pipe, (void *)ubuf, len);
        if (n == PIPE_EAGAIN)
            return block_on_pipe_locked(r, f->pipe, 0, ubuf, len, irq);
        /* Room appeared: writers may be parked waiting for it. */
        woke = n > 0 ? pipe_wake_locked(f->pipe, 1) : NULL;
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        if (woke)
            preempt_if_higher(woke);
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
        if (n > 0) {
            /* The descriptor, not the littlefs handle, is what lseek() and
             * fd_clone() speak: keep the mirror in step so a write followed
             * by lseek(SEEK_CUR) lands where the bytes ended. */
            f->pos += (uint32_t)n;
            if (f->pos > f->size)
                f->size = f->pos;
        }
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
        uint32_t irq = plat_irq_save();
        proc_t *woke;
        int n;

        spin_lock(&proc_lock);
        n = pipe_write(f->pipe, (const void *)ubuf, len);
        if (n == PIPE_EAGAIN)
            return block_on_pipe_locked(r, f->pipe, 1, ubuf, len, irq);
        /* Data for a sleeping reader - delivered before the lock goes, so
         * nobody can drain it in between and leave the reader parked. */
        woke = n > 0 ? pipe_wake_locked(f->pipe, 0) : NULL;
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        if (woke)
            preempt_if_higher(woke);
        return done(r, (uint32_t)(n == PIPE_EPIPE ? -1 : n));
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
        uint32_t irq = plat_irq_save();
        proc_t *woke = NULL;
        int last;

        /* Drop the reference and look at what is left in one step: the last
         * reader going away is what turns "empty but writers exist" into
         * "no data will ever arrive", and a writer that checks in between
         * would miss it.  The free is safe because a pipe with no
         * references has no descriptor and therefore no task inside it. */
        spin_lock(&proc_lock);
        last = pipe_drop_reader(pp);
        if (!last && pipe_readers(pp) == 0)
            woke = pipe_wake_locked(pp, 1);     /* writers have nobody to write to */
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);

        if (last)
            kfree(pp);              /* no references left at all */
        if (woke)
            preempt_if_higher(woke);
        break;
    }

    case FD_PIPE_W: {
        struct pipe *pp = f->pipe;
        uint32_t irq = plat_irq_save();
        proc_t *woke = NULL;
        int last;

        spin_lock(&proc_lock);
        last = pipe_drop_writer(pp);
        if (!last && pipe_writers(pp) == 0)
            woke = pipe_wake_locked(pp, 0);     /* readers must see EOF */
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);

        if (last)
            kfree(pp);
        if (woke)
            preempt_if_higher(woke);
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

/* Duplicate onto the lowest free descriptor.  dup2() with the target chosen
 * from the table, so every type costs exactly what it costs there. */
int process_dup(int fd)
{
    proc_t *me = current;
    file_t *src = fd_slot(me, fd);
    int nfd;

    if (!src || src->type == FD_NONE)
        return -1;
    nfd = fd_alloc(me);
    if (nfd < 0)
        return -1;
    /* fd_clone() zeroes the slot before it fills it and leaves it empty when
     * it fails, so a failure needs no cleanup here. */
    if (fd_clone(&me->fds[nfd], src) < 0)
        return -1;
    return nfd;
}

/* Move a regular file's position.  The position lives in the descriptor and
 * the littlefs handle has to follow it, because read() advances the handle
 * itself.  A pipe or a device has no position, and a seek past either end of
 * the file is refused: this is the drive-a-file subset, not a way to grow
 * one. */
int process_lseek(int fd, int off, int whence)
{
    proc_t *me = current;
    file_t *f = fd_slot(me, fd);
    int32_t base, pos;

    if (!f || f->type != FD_FILE)
        return -1;

    switch (whence) {
    case 0:  base = 0;                 break;   /* SEEK_SET */
    case 1:  base = (int32_t)f->pos;   break;   /* SEEK_CUR */
    case 2:  base = (int32_t)f->size;  break;   /* SEEK_END */
    default:
        return -1;
    }
    pos = base + off;
    if (pos < 0 || (uint32_t)pos > f->size)
        return -1;
    if (ramdisk_seek(f->fh, (uint32_t)pos) < 0)
        return -1;
    f->pos = (uint32_t)pos;
    return pos;
}

/* Describe an open descriptor the way path_stat() describes its path, so a
 * program that has the fd does not need to remember the name it opened. */
int process_fstat(int fd, uint32_t *size, uint8_t *type)
{
    proc_t *me = current;
    file_t *f = fd_slot(me, fd);

    if (!f || f->type == FD_NONE)
        return -1;
    if (f->type == FD_FILE) {
        *size = f->size;
        *type = TOS_FTYPE_REG;
    } else {
        /* Console, pipe, /dev or /proc: a character stream, which is what
         * stat() says about the same node too. */
        *size = 0;
        *type = TOS_FTYPE_CHR;
    }
    return 0;
}

/* Readiness for poll().  Only the console can be "not yet": /proc and /dev
 * produce their text on demand, a file has a position and an EOF, and a pipe
 * has a count.  A regular file reports both directions because read() at EOF
 * returns 0 at once and write() either takes the bytes or fails at once. */
short process_poll(int fd, short events)
{
    proc_t *me = current;
    file_t *f = fd_slot(me, fd);
    short rev = 0;

    if (!f || f->type == FD_NONE)
        return TOS_POLLNVAL;

    switch (f->type) {
    case FD_CONSOLE:
        if (fd == 0) {
            if (tty_is_raw() ? tty_raw_ready() : tty_line_ready())
                rev |= TOS_POLLIN;
        } else {
            rev |= TOS_POLLOUT;
        }
        break;

    case FD_FILE:
        rev |= TOS_POLLIN | TOS_POLLOUT;
        break;

    case FD_CHRDEV:
        rev |= TOS_POLLIN | TOS_POLLOUT;
        break;

    case FD_PIPE_R: {
        uint32_t irq = plat_irq_save();

        spin_lock(&proc_lock);
        if (pipe_readable(f->pipe))
            rev |= TOS_POLLIN;
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        break;
    }

    case FD_PIPE_W: {
        uint32_t irq = plat_irq_save();

        spin_lock(&proc_lock);
        if (pipe_writable(f->pipe))
            rev |= TOS_POLLOUT;
        spin_unlock(&proc_lock);
        plat_irq_restore(irq);
        break;
    }

    default:
        break;
    }

    return rev & (events | TOS_POLLERR | TOS_POLLNVAL);
}

void process_start_shell(const char *path)
{
    /* Published already claimed: see proc_create_claim().  On an SMP build
     * the other core's idle loop is scanning for READY tasks from the moment
     * cpu_init() returns, and the shell must not be one of them. */
    proc_t *p = proc_create_claim(path, NULL, 0, 1);
    if (!p)
        panic("cannot load initial program '%s'", path);

    init_proc = p;

    plat_set_kernel_stack(KSTACK_TOP);
    apply_protection(p);

    /* Straight into the frame proc_create prepared. */
    plat_enter_user(&p->saved);
}
