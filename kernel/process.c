#include "process.h"
#include "file.h"
#include "fd_internal.h"
#include "tty.h"
#include "elf.h"
#include "ramdisk.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "mem_domain.h"
#include "lz4.h"
#include "exec_format.h"
#include "kassert.h"
#include "pipe.h"
#include "devfs.h"
#include "mailbox.h"
#include "platform.h"
#include "cpu.h"
#include "tos.h"
#include "u32math.h"
#include "win_layout.h"

/* Off by default; `exec_mock = true` in a test config defines it to 1. */
#ifndef TOS_EXEC_MOCK
#define TOS_EXEC_MOCK 0
#endif

/* Table sizes, measured with -Xclang -fdump-record-layouts (2026-11-26, E1b):
 *   rv32  thread_t 256 B (264 B with TOS_KTHREADS)   proc_t 484 B
 *   arm   thread_t 192 B (200 B)                     proc_t 484 B
 * Before the split every slot paid for the whole structure - 712 B on rv32,
 * 648 B on ARM - because fds[12], cwd[64] and the identity were inline even
 * for a worker.  The split makes a worker cheap but the table is now sized in
 * threads (MAX_THREADS below), so .bss grows with the default 16 processes:
 * +4.4 KiB on rv32, +3.4 KiB on ARM, +2.2 KiB on the C3 (8 processes).  The
 * numbers and the reasoning are in docs/PROC-THREAD-MODEL.md.
 * max_procs is a menuconfig knob: 4 is the floor for a shell plus one child,
 * 16 keeps the historical default for boards that do not override it. */
#ifndef TOS_MAX_PROCS
#define TOS_MAX_PROCS 16
#endif
#define MAX_PROCS   TOS_MAX_PROCS
#ifndef TOS_MAX_THREADS
#if TOS_MAX_PROCS * 2 < 16
#define TOS_MAX_THREADS 16
#elif TOS_MAX_PROCS * 2 > 32
#define TOS_MAX_THREADS 32
#else
#define TOS_MAX_THREADS (TOS_MAX_PROCS * 2)
#endif
#endif
#define MAX_THREADS TOS_MAX_THREADS
#define MAX_ARGS    12

/* Unwaited exit codes a parent remembers (see lag_pid/lag_code).  Costs
 * LAG_MAX * 8 bytes per table slot; eight is what a test bench that spawns
 * one child per CPU times two needs, and the shell needs two. */
#define LAG_MAX     8

/* The board's default stack size, from layout.c.  It is what a task gets
 * when its image does not ask for anything else; a program can ask with
 * TOS_STACK() (see elf_tos_note) and the answer lives in thread_t.stack_size,
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
 * task_release() needs it too, and thread_t is not a type yet here. */
static uint32_t stack_used(const struct thread *p);

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
 * not enforced there; the build defines this for atk-dnr2350am-rv32 only. */

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
#ifdef TOS_KTHREADS
    ST_WAIT_THREAD, /* a join() waiting for a sibling                */
    ST_WAIT_MUTEX,  /* blocked on a user mutex word (process.c)      */
    ST_THREAD_ZOMBIE,/* exited, code kept until join                  */
    ST_DYING,       /* leader exited; threads are still winding down */
#endif
    ST_KT_WAIT,     /* kernel thread blocked by the WiFi OSI layer */
    ST_KILLING,     /* SIGKILL is releasing it outside proc_lock; not runnable */
    /* A slot claimed by proc_alloc() and still being filled in.  It has to
     * exist as a state of its own: on SMP two exec()s can be building two
     * tasks at the same time, and a slot that is only "not FREE yet" is
     * what keeps them from choosing the same one.  The scheduler never
     * picks it (it only looks at ST_READY), and proc_create() publishes
     * ST_READY as its last act, under the table lock. */
    ST_NEW,
};

/* ---- EPROC: one entry per process, and what it owns -------------------
 *
 * E1b of the split (docs/PROC-THREAD-MODEL.md): the window, its heap, the
 * descriptor table and the working directory are per process and live here;
 * a thread points at its process with t->proc and owns none of it.  Before
 * this, every slot carried copies of fds (MAX_FDS * sizeof(file_t)) and cwd
 * (TOS_PATH_MAX) that only a process leader could legally use - most of a
 * slot on the small ports and pure cost for a worker.
 *
 * The two tables are independent: procs[] is indexed by its own scan and a
 * thread reaches its entry through a pointer, so nothing has to be kept in
 * sync by index.  Process identity (pid/parent/exit/lag/name) still rides
 * the main thread until E2 moves the lifecycle over; state here only says
 * whether the entry is owned.
 */
typedef struct proc {
    uint8_t  state;              /* 0 = free, 1 = owned                */

    /* Identity.  A process has one pid and one name; every thread of it
     * answers with these, and the thread's own id is its tid.  A kernel
     * thread has no process, so it answers with its tid and keeps its name
     * in the stack pool. */
    int      pid;
    char     name[12];
    struct proc *parent;         /* NULL for init (and for an orphan)   */
    int      exit_code;          /* the leader's, handed to wait()      */
    uint16_t live_threads;       /* workers that have not exited yet    */
    int      stop_pending;       /* SIGSTOP: park every thread          */

    spin_t   glock;              /* brk + fd table + cwd                */
    uint32_t win_base, win_size, wx_start;
    uint32_t heap_base, heap_end, brk;
    file_t   fds[MAX_FDS];
    char     cwd[TOS_PATH_MAX];

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
} proc_t;

typedef struct thread {
    struct proc *proc;       /* NULL = kernel thread; else its process */
    int      state;
    int      prio;          /* PRIO_*, 0 = highest */
    int      tid;           /* this thread's id; the process id is
                             * proc->pid and is what wait() names */
    int      wait_pid;      /* ST_WAIT_CHILD/ST_WAIT_THREAD: the id this
                             * thread waits for, -1 for any child */

    uint32_t in_buf;        /* ST_WAIT_INPUT: where the line goes */
    uint32_t in_len;
    uint64_t wake_tick;     /* ST_WAIT_SLEEP */
    uint64_t cpu_ms;        /* CPU time actually spent on the CPU */
    uint32_t cpu_ticks;     /* whole ticks spent running, see cpu_ms */

    /* The window, heap and break are the process's (struct proc); only the
     * thread's own stack stays on the slot. */
    uint8_t  is_ktask;      /* runs in M-mode on its own stack, no process */
    uint8_t  kstack_slot;   /* index into the ktask stack pool          */
    uint32_t stack_top;
    uint32_t stack_size;    /* this task's stack right now: the board's
                             * default, its own TOS_STACK() request, or what
                             * a stack fault grew it to (see
                             * process_stack_grow) */

    /* A process is a leader plus its workers.  A worker points at its
     * leader and at the process through `proc`; the leader has
     * leader == NULL.  Kernel threads are marked by proc == NULL.  Code,
     * data, heap, fds, cwd and the process identity belong to the process;
     * the slot carries only the thread's saved frame, its stack, its exit
     * code for join() and its scheduling state. */
    struct thread *leader;  /* the process's main thread; NULL on it   */
    int      exit_code;     /* ST_THREAD_ZOMBIE: value for join()      */
    /* The process's glock lives in struct proc (t->proc->glock). */
#ifdef TOS_KTHREADS
    /* User-mutex state.  `held_mutex` is the word this thread owns through
     * kmutex_lock()/try(); it is what a dying thread hands on. */
    volatile int *held_mutex;
    volatile int *wmutex;
#endif
    uint64_t start_ns;      /* uptime when created, for a ps %CPU column */
    int      kill_pending;  /* SIGKILL aimed at the task on the CPU: die at
                             * the trap exit, see process_suspend() */

    /* Which CPU is running this task, -1 when it is off-CPU.  Bookkeeping
     * for ps on an SMP build: a task's state says "running" but only the
     * CPU field says whether it is this one or the other.  Maintained by
     * whichever scheduler put it there, and only ever read for display. */
    int      cpu;
    /* Where this task may run: -1 anywhere, otherwise a CPU id.  A kernel
     * thread that owns a device has to stay on the hart that owns it - the
     * USB pump belongs to core 0 - and a task the scheduler picks on the
     * wrong CPU would take the device with it.  Ordinary tasks never set
     * this. */
    int      home_cpu;

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
} thread_t;

/* The id a syscall or /proc reports for this task: the process id for a
 * user thread, its own id for a kernel thread (which has no process). */
static int thread_pid(const thread_t *t)
{
    return t->proc ? t->proc->pid : t->tid;
}

/* One setter for task state.  A direct `p->state = ...` silently allows
 * states that make no sense (FREE->READY is the expensive one: the slot is
 * handed out again while the old task still has a window).  The table is
 * deliberately broad where the existing paths legitimately jump, but it
 * rejects resurrection from FREE and unknown targets. */
static int proc_state_transition_ok(int from, int to)
{
    if (from == to)
        return 1;
    if (from == ST_FREE)
        return to == ST_NEW;
    if (to == ST_FREE)
        return from != ST_FREE;
    if (to == ST_NEW)
        return 0;
    if (to == ST_KILLING)
        return 1;
    if (from == ST_KILLING)
        return to == ST_FREE || to == ST_READY;
#ifdef TOS_KTHREADS
    if (from == ST_DYING)
        return to == ST_FREE || to == ST_READY;
    if (from == ST_THREAD_ZOMBIE)
        return to == ST_FREE || to == ST_READY;
#endif
    if (from == ST_NEW)
        return to == ST_READY || to == ST_RUNNING || to == ST_KT_WAIT;
    if (to == ST_READY)
        return from != ST_FREE;
    if (to == ST_RUNNING)
        return from == ST_READY || from == ST_STOPPED;
    if (to == ST_STOPPED)
        return from == ST_READY || from == ST_RUNNING;
    if (to == ST_WAIT_CHILD || to == ST_WAIT_INPUT ||
        to == ST_WAIT_SLEEP || to == ST_WAIT_PIPE ||
#ifdef TOS_KTHREADS
        to == ST_WAIT_THREAD || to == ST_WAIT_MUTEX ||
#endif
        to == ST_KT_WAIT)
        return from == ST_RUNNING || from == ST_READY;
#ifdef TOS_KTHREADS
    if (to == ST_THREAD_ZOMBIE)
        return from == ST_RUNNING;
    if (to == ST_DYING)
        return from == ST_RUNNING || from == ST_READY;
#endif
    return 0;
}

static void proc_set_state(thread_t *p, int st)
{
    /* Called from sched_switch(), which runs inside the tick interrupt and
     * from paths that already hold proc_lock with interrupts masked.  A
     * printing assert here could deadlock on the console lock, so halt
     * without printing: a broken transition is unrecoverable anyway. */
    KASSERT_HALT(proc_state_transition_ok(p->state, st));
    p->state = st;
}


static thread_t threads[MAX_THREADS];
static proc_t procs[MAX_PROCS];
static thread_t *init_proc;
static int next_pid = 1;
/* 1000 / tick_hz, computed once: the tick rate is fixed after boot and the
 * old expression did a 32-bit division on every tick (__udivsi3 on RV32). */
static uint32_t ms_per_tick;

/* Trap entry cannot trust mstatus.MPP to tell it where it came from: mret
 * always writes MPP=U, so a kernel thread entered through a trap frame would
 * look like a user task on its *next* trap and the entry would switch to the
 * kernel stack while the frame is on the ktask stack.
 *
 * The state is per hart.  A single global flag meant a ktask on core 0 made
 * core 1's user trap take the kernel-stack path, which panicked with
 * "kernel stack out of bounds" on rv32smp as soon as the ktask selftest ran.
 * start.S reaches this through mscratch, so the layout is part of the trap
 * contract: offset 0 = running, 4/8 = the entry's saved t1/t2.  The scheduler
 * sets it; trap_handler asks this hart's flag and forces the saved frame's
 * MPP back to M before mret.
 *
 * ktask_entry_sp stays an array indexed by mhartid: the bootstrap stub in
 * start.S reads it to get off the frame's stack and onto the ktask's own
 * stack, and each hart must see its own. */
#if defined(__riscv)
typedef struct {
    volatile int running;       /* offset 0: read by TRAP_ENTRY        */
    uint32_t t1_save;           /* offset 4: entry scratch             */
    uint32_t t2_save;           /* offset 8                            */
} ktask_cpu_t;

static ktask_cpu_t ktask_cpu[MAX_CPUS];
uint32_t ktask_entry_sp[MAX_CPUS];      /* start.S, indexed by mhartid   */

void process_ktask_trap_init(void)
{
    int id = cpu_id();

    __asm__ volatile("csrw mscratch, %0" :: "r"(&ktask_cpu[id]));
}

int process_ktask_running(void)
{
    return ktask_cpu[cpu_id()].running;
}

#define KTASK_SET_RUN(v)  (ktask_cpu[cpu_id()].running = (v))
#define KTASK_SET_SP(v)   (ktask_entry_sp[cpu_id()] = (v))
#else
static int ktask_dummy_run;

int process_ktask_running(void) { return 0; }
void process_ktask_trap_init(void) { }
#define KTASK_SET_RUN(v)  (ktask_dummy_run = (v))
#define KTASK_SET_SP(v)   ((void)0)
#endif

/* Kernel-thread stacks.  Four slots of 6 KiB; see the layout comment in
 * board/esp32c3-generic/layout.c for the 160 KiB kernel-region budget.  The ktask
 * machinery is RV32-only: it rides the M-mode trap path and start.S. */
#if defined(__riscv)
#define KTASK_SLOTS       6
/* Slot size.  Overridable so an experiment can build a smaller pool; the
 * point of the measurement below is to find the number instead of guessing.
 * Note that the request in process_ktask_create() only *records* a size - a
 * ktask always gets a whole slot, because the pool is a uniform array. */
#ifndef KTASK_STACK_BYTES
#define KTASK_STACK_BYTES 6144u
#endif
static uint8_t ktask_stacks[KTASK_SLOTS][KTASK_STACK_BYTES]
    __attribute__((aligned(16)));
static uint32_t ktask_stack_used;
/* Kernel-thread names, one per stack slot: a kernel thread has no process to
 * keep a name in, so the name lives next to the stack (see
 * docs/PROC-THREAD-MODEL.md).  The pointer belongs to the caller, which
 * passes a literal or a static string. */
static const char *ktask_names[KTASK_SLOTS];
/* High-water mark per slot, in bytes below the slot top.  Measured by painting
 * the slot with KTASK_PAINT and scanning for the last byte the thread did not
 * touch.  M-mode kernel threads have no guard of their own (no MPU rule on the
 * kernel side of RV32), so this is the only way to know whether a size is
 * enough - and the only warning if it is not. */
static uint32_t ktask_stack_hw[KTASK_SLOTS];
#define KTASK_PAINT 0xA5u

void ktask_trampoline(void (*fn)(void *), void *arg);
static void ktask_exit(void) __attribute__((noreturn));
extern void ktask_start_stub(void);
#else
/* No kernel-thread pool on this port (ARM): the accessor reports "none". */
int process_ktask_info(int idx, const char **name, uint32_t *size,
                       uint32_t *used, uint32_t *hw)
{
    (void)idx; (void)name; (void)size; (void)used; (void)hw;
    return 0;
}
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
 * the scheduler hands the lock back before it idles.  See docs/CONCURRENCY.md
 * for the global lock table this local rule belongs to.
 *
 * Every acquisition masks interrupts, because the timer tick takes this lock
 * itself (it reaps, it wakes console readers).  A section taken with them on
 * would be a self-deadlock the moment a tick landed inside it, so use
 * plat_spin_lock_irqsave()/plat_spin_unlock_irqrestore() unless the site says
 * in a comment why it cannot - the three that do are sched_switch(), which has
 * to keep the lock and the mask as separate lifetimes, cpu_take_task() and
 * process_ktask_create(), both of which are entered with interrupts already
 * off. */
static spin_t proc_lock = SPIN_INIT;

/* The machine is on its way down: init has exited and the boot CPU is in
 * plat_shutdown().  Without this, another hart that finds an empty run queue
 * in that instant cannot tell "the system is going down" from "the scheduler
 * lost every task", and panics on a machine that is shutting down exactly as
 * asked.  Set (under proc_lock) before init's slot is freed, so a CPU that
 * sees the empty queue is guaranteed to see the flag too. */
static volatile int sys_down;

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

/* align_up / add_u32_ok / align_up_ok now live in u32math.h, shared with the
 * host tests that check their edge cases without booting the kernel. */

/* Look a task up by pid.  Out of line on purpose: -O2 turns a sixteen-step
 * loop over a static array into ~300 bytes of unrolled code, and there are
 * two callers (signals, setprio).  One copy is cheaper than both. */
static thread_t *proc_by_pid(int pid) __attribute__((noinline));

static thread_t *proc_by_pid(int pid)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = &threads[i];

        /* ST_NEW is a slot claimed by an exec() that has not picked an id
         * yet: it still holds the previous task's number and must not be
         * mistaken for it. */
        if (t->state == ST_FREE || t->state == ST_NEW)
            continue;
        /* The process's main thread carries the process id as its tid, so
         * the first test is the common one; the second makes a process
         * findable by its pid even if that ever stops being true. */
        if (t->tid == pid || (t->proc && t->proc->pid == pid))
            return t;
    }
    return NULL;
}

int process_pid_exists(int pid)
{
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    thread_t *p;

    p = proc_by_pid(pid);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    return p != NULL;
}

/* ---- scheduler ------------------------------------------------------- */

/* A task more important than whoever is on the CPU must not wait for the
 * rest of that slice: need_resched makes the trap exit switch, exactly as
 * an expired quantum would. */
static void preempt_if_higher(thread_t *p)
{
    /* A task pinned to another CPU is that CPU's to run; waking it must not
     * make this one switch to a task it is not allowed to pick. */
    if (p->home_cpu >= 0 && p->home_cpu != cpu_id())
        return;
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
static void make_ready(thread_t *p, int32_t val)
{
    /* Interrupts off, not just the lock: a waker in system-call context has
     * them on, and a tick landing between the acquire and the release calls
     * this again on the same CPU, spinning on a lock it already holds. */
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    regs_t *frame = p->is_ktask && p->kframe ? p->kframe : &p->saved;

    plat_frame_set_ret(frame, (uint32_t)val);
    proc_set_state(p, ST_READY);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    preempt_if_higher(p);
}

static thread_t *sched_pick(void)
{
    int me = cpu_id();

    for (int lvl = 0; lvl < PRIO_LEVELS; lvl++) {
        for (int i = 1; i <= MAX_THREADS; i++) {
            int idx = (rr + i) % MAX_THREADS;
            thread_t *p = &threads[idx];

            if (p->state != ST_READY || p->prio != lvl)
                continue;
            /* A pinned task belongs to one CPU (the USB pump owns core 0 and
             * the controller does not follow it anywhere else).  Skipping it
             * here rather than refusing later means a wrong-CPU pick simply
             * continues down the queue. */
            if (p->home_cpu >= 0 && p->home_cpu != me)
                continue;
            rr = idx;
            return p;
        }
    }
    return NULL;
}

/* READY tasks other than `skip`.  Called with the table lock held. */
static int ready_others(const thread_t *skip)
{
    int n = 0;

    for (int i = 0; i < MAX_THREADS; i++)
        if (&threads[i] != skip && threads[i].state == ST_READY)
            n++;
    return n;
}

static int proc_waiting(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        int st = threads[i].state;
        if (st == ST_WAIT_CHILD || st == ST_WAIT_INPUT || st == ST_WAIT_SLEEP ||
            st == ST_WAIT_PIPE
#ifdef TOS_KTHREADS
            || st == ST_WAIT_THREAD || st == ST_WAIT_MUTEX
#endif
            || st == ST_KT_WAIT)
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
        thread_t *p = cpus[i].cur;

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

/* Boards may pump a device state machine here.  It runs in thread context
 * with interrupts on, which is where a polled controller belongs: a stuck
 * poll then costs a slice, not the console lock the tick holds.
 * A plain pointer, not a weak symbol: an undefined weak symbol on RV32 with
 * the medany code model links to a non-zero address, so "if (hook)" is true
 * and the call jumps into nothing (same trap as TinyUSB's
 * usbd_app_driver_get_cb). */
void (*g_idle_poll)(void);

/* Named for its first caller, the idle loop below, but it is the board's
 * deferred-work hook: a port may also run it from the boundary where the
 * kernel is about to return to Thread mode (ARM: PendSV, see
 * arch/arm/platform.c).  That second caller is what keeps a polled
 * controller alive while some task is runnable all the time - the idle loop
 * never runs then, and with USB CDC on the RP2350 a long console write used
 * to stop after about two packets because tud_task() was never reached
 * (issue #46).  Both callers are kernel context with no lock held; the hook
 * must not sleep or take the scheduler. */
int board_poll_installed(void)
{
    return g_idle_poll != 0;
}

void board_poll_deferred(void)
{
    if (g_idle_poll)
        g_idle_poll();
}

static void sched_idle(void)
{
    plat_irq_enable();
    board_poll_deferred();

    if (cpu_id() == 0) {
        /* This block runs masked, so a core that wedges between here and
         * plat_irq_enable() stops ticking and can never answer the flash
         * park handshake.  TOS_DIAG makes g_idle_site name the step it never
         * left (see kernel/diag.h). */
        plat_irq_disable();
#if TOS_DIAG
        g_idle_site = 1;
#endif
        tty_poll();
#if TOS_DIAG
        g_idle_site = 2;
#endif
        console_echo_tick();
#if TOS_DIAG
        g_idle_site = 3;
#endif
        plat_irq_enable();
#if TOS_DIAG
        g_idle_site = 0;
#endif
        process_console_kick();
    }
    plat_cpu_idle();
}

/* A process is one or more threads that share a window.  The leader owns it
 * and a worker has none of its own, so the protection unit is programmed for
 * the leader.  Thread stacks live inside the group's heap/data range (the
 * thread library allocates them with sbrk), so they are covered by the same
 * data region and need no fourth PMP/MPU entry.
 *
 * PROC_GROUP() answers "which thread speaks for the process"; a kernel
 * thread answers itself and owns nothing. */
#define PROC_GROUP(p) ((p)->leader ? (p)->leader : (p))

#define PROC_OF(p) PROC_GROUP(p)

#ifdef TOS_KTHREADS
#define PROC_IS_THREAD(p) ((p)->leader != NULL)
#else
#define PROC_IS_THREAD(p) 0
#endif

/* The process's resources: window and heap break, descriptor table, cwd.
 * One pointer, so nothing is kept in sync by index; a kernel thread has
 * proc == NULL and reaches none of it (see thread_is_kernel()). */
#define PROC_RES(p) ((p)->proc)

/* Descriptors are process state, not thread state: every thread of a group
 * sees the same open files and pipes, and a close() in one is a close() for
 * all of them (POSIX). */
#define PFD(p) ((p)->proc->fds)

/* Serialises the group's mutable state: the heap break and the descriptor
 * table.  On a uniprocessor spin_lock()/spin_unlock() compile to nothing, so
 * this costs the single-core build exactly zero; on SMP it is what keeps two
 * threads from handing out the same address or descriptor slot.
 *
 * Plain spin_lock(), unlike proc_lock: every caller is a syscall handler, so
 * interrupts are already masked, and nothing in interrupt context touches a
 * descriptor table or a heap break.  If one ever does, this becomes the
 * irqsave pair too. */
#ifdef TOS_KTHREADS
static void group_lock(thread_t *p)   { spin_lock(&p->proc->glock); }
static void group_unlock(thread_t *p) { spin_unlock(&p->proc->glock); }

/* Defined with the thread code further down; process_exit() and task_kill()
 * need it earlier. */
static void begin_group_exit_locked(thread_t *leader, int code);
/* Likewise the mutex handoff: process_thread_exit() releases a word its
 * thread held, and the implementation sits next to the syscall handlers. */
static thread_t *mutex_handoff_locked(volatile int *word, const thread_t *dead);
#else
#define group_lock(p)   ((void)0)
#define group_unlock(p) ((void)0)
#endif

/* Scheduler accounting exposed by SYS_SCHEDSTAT, so a benchmark can ask
 * "how many switches did that phase actually cost" instead of assuming one
 * per tick.  Uniprocessor counters; on SMP they still add up, which is what
 * a whole-machine measurement wants.  Compiled in with the thread feature
 * so a build without it pays neither the counters nor the syscall. */
#ifdef TOS_KTHREADS
static volatile uint32_t sched_switches;  /* picked a different task */
static volatile uint32_t sched_self;      /* scheduler ran, same task   */
static volatile uint32_t sched_ticks;     /* timer ticks seen           */
static volatile uint32_t sched_prot;      /* protection units programmed */
#endif

static void apply_protection(thread_t *p)
{
    thread_t *g = PROC_GROUP(p);
    cpu_t *c = cpu_self();
    plat_seg_t segs[PLAT_SEG_MAX];
    uint32_t win_base, win_end, stack_low, brk;
    int n = 0;

    /* Kernel threads own no address space: there is nothing to program, and
     * the previously programmed window belongs to the task we switched away
     * from.  The ports that run ktasks protect user windows only. */
    if (!g->proc)
        return;

    win_base = g->proc->win_base;
    win_end = win_base + g->proc->win_size;
    stack_low = g->stack_top - g->stack_size;
    brk = g->proc->brk;

    /* The regions describe the process, not the thread: two threads of one
     * group have the same window, break and stack, so a switch between them
     * has nothing to program.  The comparison is over the inputs themselves,
     * so a moved break or a grown stack is still programmed; see cpu_t.prot
     * for why that is stronger than a generation counter. */
    if (c->prot_valid &&
        c->prot.win_base == win_base && c->prot.win_size == PROC_RES(g)->win_size &&
        c->prot.wx_start == PROC_RES(g)->wx_start && c->prot.brk == brk &&
        c->prot.stack_top == g->stack_top &&
        c->prot.stack_size == g->stack_size)
        return;

    /* Three pieces, in address order, and any of them may be empty.  The gap
     * between the break and stack_low is left out on purpose - it is the hole
     * `prot heap` and `prot stack` probe - so the pieces are not a partition
     * of the window, and a port must not join them. */
    if (PROC_RES(g)->wx_start > win_base) {
        segs[n].base = win_base;
        segs[n].top  = PROC_RES(g)->wx_start;
        segs[n].attr = PLAT_SEG_CODE;
        n++;
    }
    if (brk > stack_low)
        brk = stack_low;                /* stale break: never overlap the stack */
    if (brk > PROC_RES(g)->wx_start) {
        segs[n].base = PROC_RES(g)->wx_start;
        segs[n].top  = brk;
        segs[n].attr = PLAT_SEG_DATA;
        n++;
    }
    if (stack_low < win_end) {
        segs[n].base = stack_low;
        segs[n].top  = win_end;         /* stack_top: they are the same value */
        segs[n].attr = PLAT_SEG_DATA;
        n++;
    }

    plat_task_protect(segs, n);

    c->prot.win_base = win_base;
    c->prot.win_size = PROC_RES(g)->win_size;
    c->prot.wx_start = PROC_RES(g)->wx_start;
    c->prot.brk = brk;
    c->prot.stack_top = g->stack_top;
    c->prot.stack_size = g->stack_size;
    c->prot_valid = 1;
#ifdef TOS_KTHREADS
    sched_prot++;
#endif
}

/* Save the outgoing frame and return the frame to resume.  Callers hold the
 * CPU at the top of a trap, so abandoning the kernel stack below us costs
 * nothing: the next trap starts at the top again. */
static regs_t *sched_switch(regs_t *r)
{
    thread_t *prev = current;
    int first = 1;
    int requeued = 0;

    /* Somebody asked for this switch; whoever asked (a wake-up, a syscall)
       does not need to ask again once we are here. */
    need_resched = 0;

    for (;;) {
        thread_t *next;
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
                proc_set_state(prev, ST_READY);
                prev->cpu = -1;
                requeued = 1;

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
            } else if (!requeued && prev && prev->state == ST_READY &&
                       prev->cpu < 0) {
                /* Woken in the window between block_locked() clearing `cpu`
                 * and this switch.  The waker already stamped its result into
                 * prev->saved, so the task is runnable again - and NOT yet
                 * owned by anyone, which is exactly what a secondary CPU's
                 * idle loop looks for.  Scanning for a successor here then
                 * picks the same task, on this core, while the other core
                 * may be picking it too: both cores ran the same shell on two
                 * kernel stacks and the console line went to the copy that
                 * lost (measured on atk-dnr2350am-rv32-smp: `cur0=1 cur1=1`, the slot
                 * stuck in WAIT_INPUT, and every command of 16+ characters
                 * silently swallowed).
                 *
                 * Claim it back and resume it here instead of going through
                 * sched_pick(): one owner, and the frame is the one the waker
                 * patched, not the live trap frame whose r0 still holds the
                 * syscall number. */
                proc_set_state(prev, ST_RUNNING);
                prev->cpu = cpu_id();
                current = prev;
                KTASK_SET_RUN(prev->is_ktask ? 1 : 0);
                quantum = QUANTUM;
                spin_unlock(&proc_lock);
                apply_protection(prev);
                return prev->is_ktask ? (prev->kframe ? prev->kframe : &prev->saved)
                                      : &prev->saved;
            }
        }

        next = sched_pick();
        waiting = proc_waiting() || other_cpu_busy();
        if (next) {
#ifdef TOS_KTHREADS
            if (next == prev)
                sched_self++;
            else
                sched_switches++;
#endif
            proc_set_state(next, ST_RUNNING);
            next->cpu = cpu_id();
            current = next;
            KTASK_SET_RUN(next->is_ktask ? 1 : 0);
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
             * frame is in thread_t.  The stub in start.S uses the entry sp to
             * leave that frame behind before it touches a C stack. */
            if (next->is_ktask) {
                KTASK_SET_SP(next->stack_top);
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
            /* "Nothing runnable" on an SMP kernel is only a bug when the
             * neighbour is not busy with the task this core wants - so say
             * what both cores' current process is before dying.  A panic
             * that names the state it died in saves a reflash per guess. */
            {
                int p0 = -1, s0 = -1, p1 = -1, s1 = -1;

                if (cpus[0].cur) {
                    p0 = thread_pid(cpus[0].cur);
                    s0 = (int)cpus[0].cur->state;
                }
#if MAX_CPUS > 1
                if (cpus[1].cur) {
                    p1 = thread_pid(cpus[1].cur);
                    s1 = (int)cpus[1].cur->state;
                }
#endif
                kprintf("[sched] cpu %u idle: c0=%d/%d c1=%d/%d\n",
                        (unsigned)cpu_id(), p0, s0, p1, s1);
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
    thread_t *p = current;

    p->saved = *r;
    /* Woken by something unexpected?  Better a failed syscall than the
       syscall number handed back as a result. */
    plat_frame_set_ret(&p->saved, (uint32_t)-1);
    proc_set_state(p, st);
    p->cpu = -1;
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    return sched_switch(r);
}

/* Give up the CPU until somebody completes our request.  For conditions
 * whose waker cannot be raced: the tick, or a driver callback that reruns
 * every millisecond.  A one-shot waker (a dying child, a pipe writer) has to
 * call block_locked() inside the critical section that checks the condition,
 * or the wake-up can be spent before this task is sleepable. */
static regs_t *block(regs_t *r, int st)
{
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);

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
    thread_t *wake = NULL;

    /* The scan, the user-buffer write and the state change have to be one
     * critical section.  On SMP the other core can retire or reuse a slot
     * between the scan and tty_take_line(), and the stale in_buf then points
     * into a dead task's stack - measured on rpi-pico-smp as `sh` resuming with
     * its saved PC pointing at its own help string.  make_ready() cannot be
     * called here because it takes proc_lock itself; do the same three
     * stores inline. */
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);

    if (tty_is_raw()) {
        /* Raw reader: hand over whatever arrived, a keystroke at a time.
         * An editor wants the byte before the next tick, not after a
         * newline that will never come. */
        if (tty_raw_ready()) {
            for (int i = 0; i < MAX_THREADS; i++) {
                thread_t *p = &threads[i];
                if (p->state != ST_WAIT_INPUT)
                    continue;
                int n = tty_take_raw((char *)p->in_buf, (int)p->in_len);
                regs_t *frame = p->is_ktask && p->kframe ? p->kframe
                                                          : &p->saved;
                plat_frame_set_ret(frame, (uint32_t)n);
                proc_set_state(p, ST_READY);
                wake = p;
                break;
            }
        }
    } else if (tty_line_ready()) {
        for (int i = 0; i < MAX_THREADS; i++) {
            thread_t *p = &threads[i];
            if (p->state != ST_WAIT_INPUT)
                continue;
            int n = tty_take_line((char *)p->in_buf, (int)p->in_len);
            regs_t *frame = p->is_ktask && p->kframe ? p->kframe : &p->saved;
            plat_frame_set_ret(frame, (uint32_t)n);
            proc_set_state(p, ST_READY);
            wake = p;
            break;
        }
    }
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    if (wake)
        preempt_if_higher(wake);
}

void process_tick(regs_t *r)
{
    /* `current` is a macro (cpu_self()->cur), so every mention re-reads the
     * CPU slot.  Taking a snapshot is not cosmetic: the null check below and
     * the dereference that follows used to be two separate loads, and a tick
     * that lands while the slot is being cleared (exit, kill, a task killed
     * from the other core) dereferenced NULL at current->state.  Measured on
     * rpi-pico-smp as a HardFault at `ldrb r0,[r0,#0x74]` in process_tick, with
     * r0 = 0, roughly every few hundred dd/rm rounds. */
    thread_t *cur = current;
    uint64_t now;

#ifdef TOS_KTHREADS
    sched_ticks++;
#endif

    /* Cheapest moment to notice a blown kernel stack: the tick runs no
       matter what the task is doing, and r points at the trap frame on
       that very stack.  A kernel thread's frame is on its own stack. */
    /* Only a live user task owns the kernel stack.  A ktask frame is on its
     * own stack, and during SYS_KTASK_EXIT the current slot is already free
     * (or NULL) while the frame is still on that stack. */
    if (cur && !cur->is_ktask && cur->state != ST_FREE)
        kstack_check(r);

    if (cur && cur->state == ST_RUNNING) {
        cur->cpu_ticks++;
        cur->cpu_ms += ms_per_tick;
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

    /* The scan, the deadline test, the frame write and the state change are
     * one critical section.  Both CPUs take ticks, so two of them can see
     * the same sleeper as due; make_ready() only takes the lock for the
     * write and does not re-check the state, so the slower tick used to
     * turn a task the scheduler had already made RUNNING back into READY -
     * the same task on two CPUs, or a lost return value. */
    thread_t *woke = NULL;
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);

    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *p = &threads[i];

        if ((p->state == ST_WAIT_SLEEP || p->state == ST_KT_WAIT) &&
            p->wake_tick && (int64_t)(now - p->wake_tick) >= 0) {
            regs_t *frame = p->is_ktask && p->kframe ? p->kframe : &p->saved;

            plat_frame_set_ret(frame, 0);
            proc_set_state(p, ST_READY);
            if (!woke || p->prio < woke->prio)
                woke = p;
        }
    }
    /* Still under the lock: the pointer is then guaranteed to name a live
     * slot, and preempt_if_higher() only touches this CPU's request flag. */
    if (woke)
        preempt_if_higher(woke);
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    /* Preempt only a task the timer interrupted in user mode: a tick that
       lands inside a kernel call must let that call finish, because the
       kernel stack below it is not resumable. */
    if (!cur || !plat_fault_is_user(r)) {
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
thread_t *cpu_take_task(void)
{
    thread_t *p;

    plat_irq_disable();
    spin_lock(&proc_lock);
    p = sched_pick();
    if (p) {
        proc_set_state(p, ST_RUNNING);
        p->cpu = cpu_id();
        current = p;
        quantum = QUANTUM;
    }
    spin_unlock(&proc_lock);

    if (p)
        apply_protection(p);
    return p;
}

/* See process.h: thread_t is private to this file. */
regs_t *process_saved_frame(struct thread *p)
{
    return &p->saved;
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
        kprintf("\n[pid %d] killed\n", thread_pid(current));
        return process_exit(r, 128 + 9);
    }
    if (current && current->proc && current->proc->stop_pending &&
        current->state == ST_RUNNING) {
        kprintf("\n[pid %d] stopped\n", thread_pid(current));
        proc_set_state(current, ST_STOPPED);
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
/* Whether a window has to be a whole multiple of the stack size.  True when a
 * single NAPOT PMP entry (or one MPU region) describes the stack, because the
 * region's base then has to be aligned to its size.  A board whose PMP has
 * TOR matching - which has no alignment rule - can size the image region to
 * the page and give the stack its own region instead, and says so here. */
__attribute__((weak)) int plat_window_needs_stack_alignment(void)
{
    return 1;
}

static void layout_check(void)
{
    uint32_t grain = PLAT_APP_GRAIN;
    uint32_t stk = DEFAULT_STACK;
    uint32_t guard = PLAT_HEAP_GUARD;
    uint32_t area = PLAT_APP_MAX - PLAT_APP_BASE;
    uint32_t min_win;

    if (stk == 0 || (stk & (stk - 1)) != 0)
        panic("task layout: stack is %u bytes, which is not a power of two", stk);
    if (plat_window_needs_stack_alignment() && grain % stk != 0)
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
    memset(threads, 0, sizeof(threads));
    memset(procs, 0, sizeof(procs));
    current = NULL;
    init_proc = NULL;
    next_pid = 1;
    rr = 0;
    quantum = QUANTUM;
    need_resched = 0;
    ms_per_tick = 1000u / plat_tick_hz();

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
    return current ? thread_pid(current) : 0;
}

void *process_current_task(void)
{
    return current;
}

/* The cwd is per process.  When no task is current (early boot, before
 * the shell starts) the answer is the root, which is also what a fresh
 * thread slot reads as after proc_alloc()'s memset. */
const char *process_cwd_str(void)
{
    /* current is NULL before the shell starts, and it can be a kernel thread
     * (no process) while a device task runs.  Both answer with the root. */
    const char *cwd = (current && current->proc) ? current->proc->cwd : NULL;

    return (cwd && cwd[0]) ? cwd : "/";
}

void process_set_cwd_str(const char *path)
{
    char *cwd;

    if (!current || !current->proc)
        return;
    /* Per process, not per thread: a chdir() in one thread must move every
     * thread of the group (POSIX/Windows agree on this; before the seam this
     * wrote the calling slot's own copy).  The block is the group's, so the
     * shared answer follows from the storage rather than from a convention. */
    cwd = current->proc->cwd;
    strncpy(cwd, path, TOS_PATH_MAX - 1);
    cwd[TOS_PATH_MAX - 1] = '\0';
}

int process_console_reader(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == ST_WAIT_INPUT)
            return thread_pid(&threads[i]);
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
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == ST_WAIT_CHILD && threads[i].wait_pid > 0)
            return threads[i].wait_pid;
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

    if (pid > 0 && (!init_proc || pid != thread_pid(init_proc)))
        return pid;

    pid = foreground_job();
    if (pid > 0 && (!init_proc || pid != thread_pid(init_proc)))
        return pid;

    thread_t *best = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *p = &threads[i];

        if (p == init_proc || (p->state != ST_READY && p->state != ST_RUNNING))
            continue;
        if (!best || p->prio < best->prio ||
            (p->prio == best->prio && p->cpu_ms > best->cpu_ms))
            best = p;
    }
    return best ? thread_pid(best) : 0;
}

/* Validate a user pointer against the current task's window.  Without an
 * MMU this is a range check, not a security boundary: see PLATFORM-DESIGN. */
int process_user_ok(uint32_t addr, uint32_t len)
{
    if (!current)
        return 0;
    if (len == 0)
        return 1;
    /* A thread has no window of its own: its addresses are the group's.  A
     * kernel thread has no address space at all, so nothing is a valid user
     * pointer. */
    thread_t *g = PROC_GROUP(current);
    if (!g->proc)
        return 0;
    if (addr < g->proc->win_base)
        return 0;
    if (addr + len < addr)
        return 0;
    return addr + len <= g->proc->win_base + g->proc->win_size;
}

/* ---- windows ---------------------------------------------------------
 *
 * A window is sized to the image that will live in it: exec() measures the
 * ELF (span), adds the stack and the guard hole, rounds up to the board's
 * grain and asks for exactly that.  The shell ends up with 32 KiB on a C3
 * instead of the 64 KiB every task used to reserve, and the difference goes
 * to the next program.
 *
 * The live windows are the processes: each proc_t records its base and size,
 * so there is no second structure to keep in sync (and none to size for
 * boards whose user area is 112 MiB), a task's death merges its gap back for
 * free, and two tasks can never be handed overlapping memory.  The scan is
 * over MAX_PROCS entries, twice per gap - 16 processes, so "O(n^2)" is 256
 * compares.
 *
 * Caller holds no lock; everything here runs under the table lock, because
 * an exec() on the other CPU is inserting a window at the same moment.  The
 * window is recorded in the process entry *inside* that lock - reserving it
 * after the lock is dropped would let two concurrent execs pick the same
 * gap, each seeing a table that does not yet mention the other. */
static int win_alloc(thread_t *p, const struct mem_domain *dom, uint32_t bytes)
{
    uint32_t cand, end;
    int ok = 0;

    if (!dom || dom->grain == 0 || !(dom->flags & DOMAIN_EXEC))
        return -1;
    if (dom->size == 0 || dom->base + dom->size < dom->base)
        return -1;
    cand = dom->base;
    end = dom->base + dom->size;

    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    for (;;) {
        uint32_t next = end;

        /* The nearest window at or above `cand` bounds the gap we are
           standing in. */
        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].win_size != 0 && procs[i].win_base >= cand &&
                procs[i].win_base < next)
                next = procs[i].win_base;
        }

        /* Prefer the alignment that needs the fewest protection blocks (the
           largest power of two not exceeding the window); fall back to the
           grain when that wastes too much.  The C3 needs the fallback for a
           128 KiB window in a 224 KiB gap (issue #55); NAPOT/MPU boards get
           the preferred placement so rcc keeps the SPLIT map.  See
           win_layout.h for why both rules exist. */
        uint32_t base = win_place(cand, next, bytes, dom->grain);
        if (base != 0) {
            PROC_RES(p)->win_base = base;
            PROC_RES(p)->win_size = bytes;
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
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    return ok ? 0 : -1;
}

/* Longest contiguous free run, for the "no room" message: without it a
 * fragmented area reports "out of task windows" on a machine with 40 KiB
 * free, and the next person looks for a leak. */
static uint32_t win_largest_free(const struct mem_domain *dom)
{
    uint32_t cand, end, best = 0;

    if (!dom || dom->size == 0 || dom->base + dom->size < dom->base)
        return 0;
    cand = dom->base;
    end = dom->base + dom->size;

    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    for (;;) {
        uint32_t next = end;

        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].win_size != 0 && procs[i].win_base >= cand &&
                procs[i].win_base < next)
                next = procs[i].win_base;
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
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    /* Report the largest window that can actually start in this gap.  The
     * raw run length once said 224 KiB while a 128 KiB request still failed
     * under window-sized alignment, sending the next person looking for a
     * leak (issue #55).  Windows are whole grains, so round down. */
    return best - (dom->grain ? best % dom->grain : 0u);
}

/* ---- creation -------------------------------------------------------- */

static thread_t *proc_alloc(void)
{
    /* Claim the slot, not just find it: proc_create() then spends a while
     * loading the ELF and building the initial stack, and on SMP a second
     * exec() on the other CPU must not settle on the same table entry while
     * that is happening.  pid is set by the caller; the state is what makes
     * the slot owned, and ST_NEW says "owned, not runnable". */
    thread_t *p = NULL;

    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == ST_FREE) {
            p = &threads[i];
            /* Clear it here rather than in proc_create(): the caller used to
             * memset() the whole struct a hundred lines later, which wiped
             * this very claim (ST_FREE is 0) and reopened the race. */
            memset(p, 0, sizeof(*p));
            p->home_cpu = -1;       /* memset() says 0, which is a real CPU */
            proc_set_state(p, ST_NEW);
            p->cpu = -1;
            break;
        }
    }
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    return p;
}

/* One resource entry per process.  Separate from the thread slot: the main
 * thread keeps the process identity until E2, but the window, heap, fd table
 * and cwd are a table of their own now (docs/PROC-THREAD-MODEL.md).  Freed
 * by proc_retire_locked() when the main thread retires; a worker never
 * touches it. */
static proc_t *process_alloc(void)
{
    proc_t *pr = NULL;
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);

    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == 0) {
            pr = &procs[i];
            memset(pr, 0, sizeof(*pr));
            pr->state = 1;
            break;
        }
    }
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    return pr;
}

/* Hand a slot back to the table.  Two callers: a failed exec() (a claimed
 * slot that never became a task) and a dying one.  Both have to be under the
 * table lock, because on SMP the other CPU is scanning the table at the same
 * moment - a half-cleared entry can be picked up as a runnable task.
 *
 * Clearing win_size is what frees the memory: the live windows *are* the
 * table (see win_alloc), so a task that stops being a task stops holding
 * address space, and its gap merges with its neighbours on the next scan. */
static void proc_retire_locked(thread_t *p)
{
    /* Captured before the slot is cleared: only the process's main thread
     * owns the resources, and it is the one that gives them back. */
    int is_main = (p->leader == NULL && p->proc != NULL);

    proc_set_state(p, ST_FREE);
    p->tid = 0;
#ifdef TOS_KTHREADS
    p->leader = NULL;
    p->held_mutex = NULL;
    p->wmutex = NULL;
#endif
    p->cpu = -1;
    /* A thread slot owns no window.  The process's resources are freed when
     * its main thread retires; a worker retired by join must not touch them. */
    if (is_main) {
        p->proc->win_base = 0;
        p->proc->win_size = 0;
        p->proc->state = 0;
        p->proc = NULL;
    }
}

static void proc_retire(thread_t *p)
{
    /* An exec that fails after the descriptor inheritance still holds the
     * references fd_clone() took.  Retiring the slot without closing them
     * leaks pipe readers/writers: measured on mps2 as a writer count stuck
     * at 1 with no task holding the end, and a reader parked forever on a
     * pipe nobody will ever close. */
    for (int i = 0; i < MAX_FDS; i++)
        fd_close_in(p, i);
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    proc_retire_locked(p);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
}

/* Where the initial [argc][argv..][NULL] block would start for argc
 * arguments.  build_initial_stack() lays the strings out below
 * stack_top - 256 and this block below them, so the distance to the
 * initial sp is the headroom a start-up frame needs; a program that needs
 * more stack than the window has left must be refused (see process_exec). */
static uint32_t argv_block_start(thread_t *p, int argc)
{
    uint32_t sp = p->stack_top - 256;

    sp = (sp - (uint32_t)(argc + 2) * 4) & ~0xFu;
    return sp;
}

static uint32_t build_initial_stack(thread_t *p, char *const argv[], int argc)
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

static void set_name(proc_t *pr, const char *path)
{
    const char *base = path;

    for (const char *s = path; *s; s++) {
        if (*s == '/')
            base = s + 1;
    }
    int i = 0;
    while (base[i] && i < (int)sizeof(pr->name) - 1) {
        pr->name[i] = base[i];
        i++;
    }
    pr->name[i] = '\0';
}

#if defined(__riscv)
/* ---- kernel threads -------------------------------------------------- *
 *
 * A kernel thread is a thread with proc == NULL (and is_ktask set): no user
 * window, its own stack, M-mode code.  It is scheduled by the same frames as a user task,
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

/* Bytes a ktask slot has used, counted from the untouched paint at the
 * bottom.  Cheap (one pass over at most KTASK_STACK_BYTES) and only called on
 * exit or from /proc, never in the scheduler. */
static uint32_t ktask_slot_used(int slot)
{
    const uint8_t *base = ktask_stacks[slot];
    uint32_t i = 0;

    while (i < KTASK_STACK_BYTES && base[i] == KTASK_PAINT)
        i++;
    {
        uint32_t used = KTASK_STACK_BYTES - i;

        if (used > ktask_stack_hw[slot])
            ktask_stack_hw[slot] = used;
        return used;
    }
}

/* Per-slot snapshot for /proc/ktask: name, slot size, bytes used, high-water.
 * Returns 0 for a free slot.  Called from procfs (thread context), never from
 * the scheduler; the scan is bounded by KTASK_STACK_BYTES. */
int process_ktask_info(int idx, const char **name, uint32_t *size,
                       uint32_t *used, uint32_t *hw)
{
    if (idx < 0 || idx >= KTASK_SLOTS || !(ktask_stack_used & (1u << idx)))
        return 0;
    *size = KTASK_STACK_BYTES;
    *used = ktask_slot_used(idx);
    *hw = ktask_stack_hw[idx];
    /* The slot bitmap says a thread exists; the name pool next to the stacks
     * says what it is called.  A torn read is possible on SMP - the name is
     * display data and the pointer is static. */
    *name = ktask_names[idx] ? ktask_names[idx] : "ktask";
    return 1;
}

#ifdef TOS_KTASK_TEST
/* QEMU-only exerciser for the ktask machinery (S0/S4): the WiFi OSI layer is
 * the only real client and needs hardware, the USB pump needs a controller,
 * so without this the pool, the pinning, the paint/high-water and
 * /proc/ktask never run in CI.  It is a kernel thread at priority 0 pinned
 * to CPU 0; it sleeps five times through the tick, yields once, records the
 * CPU every time it wakes and stays alive (sleeping) so /proc/ktask can show
 * the slot's high-water mark. */
static volatile int ktest_cpu_bad;
static volatile int ktest_done;

static void ktest_fn(void *arg)
{
    (void)arg;

    for (int i = 0; i < 5; i++) {
        if (plat_cpu_id() != 0)
            ktest_cpu_bad++;
        process_ktask_sleep(1);
    }
    process_ktask_yield();
    if (plat_cpu_id() != 0)
        ktest_cpu_bad++;
    ktest_done = 1;
    kprintf("[ktest] ktask ran 5 sleeps + yield, cpu_bad=%d\n",
            ktest_cpu_bad);
    for (;;)
        process_ktask_sleep(1000);      /* keep the slot for /proc/ktask */
}

void process_ktask_selftest(void)
{
    int pid = process_ktask_create("ktest", ktest_fn, NULL, 4096, 0, 0);

    kprintf("[ktest] create pid=%d\n", pid);
}
#endif

int process_ktask_create(const char *name, void (*fn)(void *), void *arg,
                         uint32_t stack_bytes, int prio, int cpu)
{
    thread_t *p;
    int slot = -1;

    if (!fn)
        return -1;
    if (stack_bytes == 0 || stack_bytes > KTASK_STACK_BYTES)
        stack_bytes = KTASK_STACK_BYTES;

    /* Plain interrupt masking, not plat_spin_lock_irqsave(): the lock that
     * guards the process table is *not* held across this region.  proc_alloc()
     * takes it itself, and holding it here (which is what a single
     * lock+mask primitive would mean) would deadlock.  What this section
     * actually protects is the ktask stack bitmap; proc_lock is taken for the
     * two fields at the end, which is all the rest of the kernel reads. */
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
    p->proc = NULL;                 /* a kernel thread owns no process */
    p->kstack_slot = (uint8_t)slot;
    memset(ktask_stacks[slot], KTASK_PAINT, KTASK_STACK_BYTES);
    ktask_stack_hw[slot] = 0;
    p->prio = prio;
    p->stack_top = (uint32_t)&ktask_stacks[slot][KTASK_STACK_BYTES];
    p->stack_size = stack_bytes;
    p->cpu = -1;
    p->home_cpu = cpu < 0 ? -1 : cpu;   /* a device owner names its hart */
    p->start_ns = plat_uptime_ms();
    ktask_names[slot] = name ? name : "ktask";

    memset(&p->saved, 0, sizeof(p->saved));
    p->saved.mstatus = RV_MSTATUS_MPP_M;   /* MPIE=0: the stub enables it */
    p->saved.mepc = (uint32_t)ktask_start_stub;
    p->saved.sp = 0;                        /* unused; the stub sets sp */
    p->saved.ra = (uint32_t)ktask_exit;
    p->saved.a0 = (uint32_t)fn;
    p->saved.a1 = (uint32_t)arg;
    p->kframe = &p->saved;

    spin_lock(&proc_lock);
    p->tid = next_pid++;
    proc_set_state(p, ST_READY);
    spin_unlock(&proc_lock);
    plat_irq_restore(irq);
    return p->tid;
}

regs_t *process_ktask_syscall(regs_t *r)
{
    thread_t *p = current;

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
        proc_set_state(p, ST_KT_WAIT);
        p->cpu = -1;
        return sched_switch(r);
    }

    case SYS_KTASK_YIELD:
        p->saved = *r;
        p->kframe = r;
        plat_frame_set_ret(r, 0);
        proc_set_state(p, ST_READY);
        p->cpu = -1;
        return sched_switch(r);

    case SYS_KTASK_EXIT: {
        uint32_t irq = plat_spin_lock_irqsave(&proc_lock);

        ktask_stack_used &= ~(1u << p->kstack_slot);
        ktask_names[p->kstack_slot] = NULL;
        p->is_ktask = 0;
        p->kstack_slot = 0;
        proc_retire_locked(p);
        plat_spin_unlock_irqrestore(&proc_lock, irq);
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
/* ---- software stack guard (ports without an MPU) ----------------------- */

/* A port whose PMP/MPU faults on the guard hole does not need this: the
 * hardware stops the store itself.  One that has neither - the RP2040 - gets
 * a canary word at the top of the hole instead, planted when the window is
 * created.  A store that runs off the bottom of the stack clobbers it, and
 * the next entry into the kernel notices: the stack is grown one step into
 * the heap's tail, or the task is killed, instead of the writes continuing
 * quietly down into the image.  Nothing is checked on the way out, so a tight
 * recursion with no syscalls is only caught at its next call. */
#define USER_STACK_GUARD 0x5354414Bu               /* "STAK" */

__attribute__((weak)) int plat_user_stack_guard(void)
{
    return 0;
}

/* Slack reserved under the stack of a task that did not declare one, so the
 * first unexpected recursion can double the stack instead of dying.
 *
 * A task that carries TOS_STACK() has said how deep it goes: its window is
 * then exact, which is what keeps a resident program (the shell) at one grain
 * instead of paying for a step it will never take.
 *
 * A board with no faulting protection unit has nothing to grow from - the
 * overflow is a silent write through the heap instead of a fault - so it
 * answers 0 and keeps the windows tight. */
__attribute__((weak)) uint32_t plat_window_grow_reserve(void)
{
    /* Opt-in, because on a coarse grain the reserve is not free: with a 16 KiB
     * step, a program whose need is an exact multiple of it (hello world:
     * 4 image + 4 guard + 8 stack) jumps a whole grain the moment anything is
     * added.  Boards whose rounding already leaves a stack of slack therefore
     * keep 0 and grow opportunistically; the 4 KiB-step boards, where the
     * rounding can never leave 4 KiB, override this with their stack size. */
    return 0;
}

static int stack_request_ok(uint32_t v)
{
    if ((v & (v - 1u)) != 0 || v < PLAT_PAGE_SIZE)
        return 0;
    /* Where the stack is its own TOR region it does not have to divide the
     * grain; it only has to be able to fit in a window. */
    if (!plat_window_needs_stack_alignment())
        return v <= PLAT_APP_MAX - PLAT_APP_BASE - PLAT_HEAP_GUARD -
                    PLAT_PAGE_SIZE;
    return PLAT_APP_GRAIN % v == 0;
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

/* C2/A-3: the format tables.  A new container or image format registers by
 * appending an entry here; exec() probes in order and never names a concrete
 * handler, so adding a format is data, not a new branch.  The NULL sentinel
 * keeps the loops obvious and makes a forgotten entry a compile error. */

#if TOS_EXEC_MOCK
/* Test-only container, compiled only when the config asks for `exec_mock`:
 * "MOCKTOS\0" + u32le size + span/stack/heap (the same hint shape LZ4TOS
 * carries) + raw ELF.  Its only job is to prove that a new format is one
 * table entry and no change to exec(). */
#define MOCK_HDR 24
static uint32_t mock_probe(const char *path, uint32_t *raw_size,
                           lz4_window_hint_t *hint)
{
    uint8_t hdr[MOCK_HDR];
    uint32_t got = 0, rs;
    int h;

    *raw_size = 0;
    memset(hint, 0, sizeof(*hint));
    h = ramdisk_open(path);
    if (h < 0)
        return 0;
    while (got < MOCK_HDR) {
        int n = ramdisk_read(h, hdr + got, MOCK_HDR - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    uint32_t fsize = ramdisk_size(h);
    ramdisk_close(h);
    if (got < MOCK_HDR || memcmp(hdr, "MOCKTOS\0", 8) != 0)
        return 0;
    rs = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
         ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    if (rs == 0 || rs != fsize - MOCK_HDR)
        return 0;
    *raw_size = rs;
    hint->span      = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
                      ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    hint->stack_ask = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) |
                      ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
    hint->heap_ask  = (uint32_t)hdr[20] | ((uint32_t)hdr[21] << 8) |
                      ((uint32_t)hdr[22] << 16) | ((uint32_t)hdr[23] << 24);
    return MOCK_HDR;
}

static int mock_load(const char *path, uint32_t header, uint8_t *dst,
                     uint32_t raw_size)
{
    uint32_t got = 0;
    int h = ramdisk_open(path);
    if (h < 0)
        return -1;
    int rc = -1;
    if (ramdisk_seek(h, header) == 0) {
        while (got < raw_size) {
            int n = ramdisk_read(h, dst + got, raw_size - got);
            if (n <= 0)
                break;
            got += (uint32_t)n;
        }
        if (got == raw_size)
            rc = 0;
    }
    ramdisk_close(h);
    return rc;
}

static const struct exec_format mock_exec = {
    .name = "mocktos",
    .probe = mock_probe,
    .load = mock_load,
    .note = "test-only MOCKTOS envelope (raw ELF payload)",
    .version = 1,
};
#endif

static const struct exec_format lz4tos_exec = {
    .name = "lz4tos",
    .probe = envelope_probe,
    .load = stream_load,
    .note = "LZ4TOS envelope with 28-byte hint",
    /* V0 (flags 0x0001) and V1 (0x0101) both load; 1 is the newest. */
    .version = 1,
};

static const struct exec_format *const exec_formats[] = {
#if TOS_EXEC_MOCK
    &mock_exec,
#endif
    &lz4tos_exec,
    NULL
};

static const struct exec_image_format elf32_exec = {
    .name = "elf32",
    .probe = elf_probe,
    .load = elf_load,
    .note = "ET_DYN/ET_EXEC ELF32, segments relocated into the task window",
    /* ELF32 has no revision field of its own; 1 is this loader's first
     * contract, bump it if the segment rules ever change. */
    .version = 1,
};

static const struct exec_image_format *const exec_image_formats[] = {
    &elf32_exec,
    NULL
};

/* /proc/execfmt: what exec() can actually dispatch to, and which revision of
 * each format the handler implements.  Two tables, one line per registered
 * handler, in dispatch order.  This is the reader that keeps the `version`
 * field honest - see exec_format.h. */
uint32_t process_execfmt_text(char *buf, uint32_t cap)
{
    uint32_t off;
    int n;

    if (!buf || cap < 64)
        return 0;

    n = ksnprintf(buf, cap, "container ver note\n");
    if (n < 0)
        return 0;
    off = (uint32_t)n;

    for (int i = 0; exec_formats[i]; i++) {
        n = ksnprintf(buf + off, cap - off, "%s %u %s\n",
                      exec_formats[i]->name, (unsigned)exec_formats[i]->version,
                      exec_formats[i]->note);
        if (n < 0)
            break;
        off += (uint32_t)n;
        if (off >= cap - 1)
            break;
    }

    n = ksnprintf(buf + off, cap - off, "image ver note\n");
    if (n < 0)
        return off;
    off += (uint32_t)n;

    for (int i = 0; exec_image_formats[i]; i++) {
        n = ksnprintf(buf + off, cap - off, "%s %u %s\n",
                      exec_image_formats[i]->name,
                      (unsigned)exec_image_formats[i]->version,
                      exec_image_formats[i]->note);
        if (n < 0)
            break;
        off += (uint32_t)n;
        if (off >= cap - 1)
            break;
    }
    return off;
}

static thread_t *proc_create_claim(const char *path, char *const argv[], int argc,
                                 int claim)
{
    uint32_t raw_size = 0;
    lz4_window_hint_t hint = {0, 0, 0};
    const struct exec_format *cfmt = NULL;
    const struct mem_domain *dom = mem_domain_exec();
    uint32_t hdr = 0;

    if (!dom) {
        kprintf("exec: no executable memory domain\n");
        return NULL;
    }

    for (int i = 0; exec_formats[i]; i++) {
        hdr = exec_formats[i]->probe(path, &raw_size, &hint);
        if (hdr) {
            cfmt = exec_formats[i];
            break;
        }
    }
    if (!cfmt) {
        kprintf("exec: '%s' not found or not a loadable program\n", path);
        return NULL;
    }

    /* Claim the table slot first: win_alloc() records the window in this
       very entry, and proc_retire() gives both back on any failure below.
       Nothing here owns a heap buffer - the image lives in the window. */
    proc_t *pr = process_alloc();
    thread_t *p = proc_alloc();
    if (!pr || !p) {
        kprintf("exec: process or thread table full\n");
        if (p)
            proc_retire(p);
        if (pr)
            pr->state = 0;
        return NULL;
    }
    /* The new thread is the process's main thread: it speaks for the
     * resources from the first instruction (window, fd table, cwd). */
    p->proc = pr;

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
    uint32_t app_total = dom->size;
    uint32_t grow = hint.stack_ask ? 0 : plat_window_grow_reserve();
    if (add_u32_ok(win_need, stk, &win_need) < 0 ||
        add_u32_ok(win_need, grow, &win_need) < 0 ||
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
    if (align_up_ok(win_need, dom->grain, &win) < 0) {
        kprintf("exec: '%s' window alignment overflows\n", path);
        proc_retire(p);
        return NULL;
    }
    if (win < dom->grain)
        win = dom->grain;
    if (win_alloc(p, dom, win) != 0) {
        kprintf("exec: '%s' needs a %u KiB window, the largest free block is %u KiB\n",
                path, win / 1024u, win_largest_free(dom) / 1024u);
        proc_retire(p);
        return NULL;
    }
    uint32_t base = PROC_RES(p)->win_base;

    /* The decompressor is given raw_size, so prove the destination really
     * has that many bytes before letting it write. */
    if (raw_size > win) {
        kprintf("exec: '%s' image is larger than its window\n", path);
        proc_retire(p);
        return NULL;
    }

    /* Decode the envelope straight into the reserved window. */
    if (cfmt->load(path, hdr, (uint8_t *)base, raw_size) != 0) {
        kprintf("exec: '%s' is corrupt (bad LZ4 block)\n", path);
        proc_retire(p);
        return NULL;
    }
    uint8_t *data = (uint8_t *)base;
    uint32_t size = raw_size;

    /* Re-read what the ELF says now that it is here, and refuse a stale or
       tampered hint before the loader can write past an undersized window. */
    uint32_t link_lo = 0, link_hi = 0, link_entry = 0, link_wx = 0;
    const struct exec_image_format *ifmt = NULL;
    for (int i = 0; exec_image_formats[i]; i++) {
        if (exec_image_formats[i]->probe(data, size, &link_lo, &link_hi,
                                         &link_entry, &link_wx) == 0) {
            ifmt = exec_image_formats[i];
            break;
        }
    }
    if (!ifmt) {
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
        align_up_ok(win_elf, dom->grain, &win_elf) < 0) {
        kprintf("exec: '%s' ELF window overflows\n", path);
        proc_retire(p);
        return NULL;
    }
    if (win_elf < dom->grain)
        win_elf = dom->grain;
    if (span != hint.span || asked_stack != hint.stack_ask ||
        asked_heap != hint.heap_ask || win_elf > win || raw_size > win) {
        kprintf("exec: '%s' window hint disagrees with its ELF\n", path);
        proc_retire(p);
        return NULL;
    }

    /* The loader slides the segments into place, clears .bss, applies
       relocations and checks every write against the window it was given. */
    uint32_t entry = 0;
    if (ifmt->load(data, size, win, base, &entry) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        proc_retire(p);
        return NULL;
    }

    /* proc_alloc() already cleared this entry and marked it ST_NEW; from
     * here to the publish at the bottom it is "owned and unusable", which is
     * exactly what ST_NEW means.  Start in the root, then inherit the
     * parent's cwd so a program spawned
     * from a subdirectory lists and opens files relative to where the user
     * is, not always the root. */
    {
        char *cwd = PROC_RES(p)->cwd;

        cwd[0] = '/';
        cwd[1] = '\0';
        if (current && PROC_RES(current)->cwd[0]) {
            strncpy(cwd, PROC_RES(current)->cwd, TOS_PATH_MAX - 1);
            cwd[TOS_PATH_MAX - 1] = '\0';
        }
    }
    p->cpu = -1;        /* memset() above zeroed it; 0 is a real CPU */
    /* Pid allocation is under the table lock for the same reason the window
     * is: two exec()s on two CPUs would otherwise hand out the same number,
     * and `wait(pid)` would then reap the wrong task. */
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    /* One id space: the main thread's tid *is* the process id, which is what
     * wait() names and what ps prints.  It has no leader and no worker yet. */
    p->tid = next_pid++;
    p->proc->pid = p->tid;
    p->leader = NULL;
    p->proc->live_threads = 0;   /* workers only; see thread_create() */
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    /* Descriptors.  0/1/2 are the console by default; everything else is
     * inherited from the parent, which is the whole reason `a | b` works:
     * the shell wires fd 1 to a pipe, then exec()s the child, and the child
     * gets that wiring as part of its own table.  The same is what makes
     * `echo hi > file` work - by the time the child starts, the shell's fd 1
     * is a file, and the child inherits it. */
    for (int i = 0; i < MAX_FDS; i++)
        PFD(p)[i].type = FD_NONE;
    if (current) {
        for (int i = 0; i < MAX_FDS; i++) {
            /* Console and pipes were always inherited; files and devices
             * joined them when redirection started crossing exec().  The
             * rules live in fd_clone() so dup2() cannot drift from this. */
            if (PFD(current)[i].type == FD_NONE)
                continue;
            if (fd_clone(&PFD(p)[i], &PFD(current)[i]) < 0)
                PFD(p)[i].type = FD_NONE;
        }
    }
    for (int i = 0; i < 3; i++)
        if (PFD(p)[i].type == FD_NONE)
            PFD(p)[i].type = FD_CONSOLE;

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
    PROC_RES(p)->wx_start = base + (link_wx - link_lo);
    /* The stack occupies the very top of the window, so that its region is
       naturally aligned; the initial frame starts a little below.  Its size
       is this task's own now, and every later use - protection, the argv
       block, the poison fill, the high-water scan - goes through
       stack_size so the four cannot drift apart. */
    p->stack_top = base + win;
    p->stack_size = stk;
    PROC_RES(p)->heap_base = base + span;
    PROC_RES(p)->heap_end = p->stack_top - stk - HEAP_GUARD;
    if (plat_user_stack_guard())
        *((uint32_t *)(p->stack_top - stk) - 1) = USER_STACK_GUARD;

    /* Fill the stack with a poison byte before anything uses it, so the
       deepest address the program ever wrote to can be recovered later: the
       first byte from the bottom that is no longer 0xCC is the high-water
       mark (see stack_used()).  Done here, before build_initial_stack()
       writes argv on top, so the arguments count as usage - they are memory
       the program can see. */
    memset((void *)(p->stack_top - stk), STACK_POISON, stk);
    PROC_RES(p)->brk = PROC_RES(p)->heap_base;
    set_name(p->proc, path);

    uint32_t sp = build_initial_stack(p, argv, argc);
    if (!sp) {
        kprintf("exec: out of memory\n");
        proc_retire(p);
        return NULL;
    }

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
    plat_prepare_user_frame(&p->saved, entry, sp);

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
    irq = plat_spin_lock_irqsave(&proc_lock);
    proc_set_state(p, claim ? ST_RUNNING : ST_READY);
    if (claim) {
        p->cpu = cpu_id();
        current = p;
        quantum = QUANTUM;
    }
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    return p;
}

static thread_t *proc_create(const char *path, char *const argv[], int argc)
{
    return proc_create_claim(path, argv, argc, 0);
}

int process_exec(regs_t *r, const char *path, char *const argv[], int argc)
{
    if (!current)
        return -1;

#ifdef TOS_KTHREADS
    /* exec() replaces the address space, so it cannot be called by a thread
     * that shares one with its siblings: the new image would pull the window
     * out from under them.  POSIX solves this by killing the other threads;
     * for now the call simply fails. */
    if (current->leader)
        return -1;
#endif

    thread_t *p = proc_create(path, argv, argc);
    if (!p)
        return -1;

    /* exec spawns; it does not run the child here.  The parent keeps its
       slice and calls wait() when it wants the result.  The link is between
       processes: a worker thread may spawn, and wait() is a process call. */
    p->proc->parent = (current && current->proc) ? current->proc : NULL;
    return (int)p->proc->pid;
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
    thread_t *t;

    if (prio < 0 || prio >= PRIO_LEVELS)
        return -1;
    if (current && prio < current->prio && current != init_proc)
        return -1;

    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    t = pid == 0 ? current : proc_by_pid(pid);
    if (!t) {
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return -1;
    }

    t->prio = prio;
    if (t != current && t->state == ST_READY)
        preempt_if_higher(t);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
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
static void task_release(thread_t *p)
{
#ifdef TOS_KTHREADS
    /* A thread owns no window, no children and (for now) only console fds.
     * The group leader does the real teardown. */
    if (p->leader)
        return;
#endif

    uint32_t used = stack_used(p);

    if (used > stack_peak.bytes) {
        stack_peak.bytes = used;
        for (uint32_t i = 0; i + 1 < sizeof(stack_peak.name); i++) {
            stack_peak.name[i] = p->proc->name[i];
            if (!p->proc->name[i])
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
#ifdef TOS_PIO
    /* A PIO state machine is system hardware: stop the ones this task owns
     * and put the pins back, or the next task inherits a running SM. */
    plat_pio_release(thread_pid(p));
#endif
}

/* Remember a child's exit code for a wait() that has not happened yet.
 * Called with the table lock held. */
static void lag_push(proc_t *pr, int pid, int code)
{
    if (pr->lag_n >= LAG_MAX)
        return;                 /* the oldest is lost; see lag_pid/lag_code */
    pr->lag_pid[pr->lag_n] = pid;
    pr->lag_code[pr->lag_n] = code;
    pr->lag_n++;
}

/* Take the code of child `pid` (any child when pid < 0).  Returns 0 when
 * that child is not in the list.  Called with the table lock held. */
static int lag_take(proc_t *pr, int pid, int *code)
{
    for (int i = 0; i < pr->lag_n; i++) {
        if (pid >= 0 && pr->lag_pid[i] != pid)
            continue;
        *code = pr->lag_code[i];
        pr->lag_n--;            /* order does not matter, so fill the hole */
        pr->lag_pid[i] = pr->lag_pid[pr->lag_n];
        pr->lag_code[i] = pr->lag_code[pr->lag_n];
        return 1;
    }
    return 0;
}

/* The thread of `pr` blocked in wait() for child `pid` (any child when
 * pid < 0), or NULL.  The waiter is a *thread*: any thread of a process may
 * call wait(), and it is that thread the code is delivered to.  Called with
 * proc_lock held. */
static thread_t *wait_waiter_locked(proc_t *pr, int pid)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *q = &threads[i];

        if (q->state != ST_WAIT_CHILD || q->proc != pr)
            continue;
        if (pid >= 0 && q->wait_pid != pid)
            continue;
        return q;
    }
    return NULL;
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
static thread_t *reap_into_parent_locked(thread_t *p, int code)
{
    proc_t *cp = p->proc;
    proc_t *pp;
    thread_t *woke = NULL;

    if (!cp)
        return NULL;            /* a kernel thread has no parent or children */

    pp = cp->parent;
    if (pp) {
        thread_t *w = wait_waiter_locked(pp, cp->pid);

        if (w) {
            /* Same as make_ready(), but the lock is already held and this
             * must not release it: the frame write and the state store stay
             * in the sender's critical section. */
            plat_frame_set_ret(&w->saved, (uint32_t)code);
            proc_set_state(w, ST_READY);
            woke = w;
        } else {
            lag_push(pp, cp->pid, code);
        }
    }

    /* Children of the dying process go to its parent, or to init. */
    for (int i = 0; i < MAX_THREADS; i++) {
        proc_t *c = threads[i].proc;

        if (threads[i].state != ST_FREE && c && c->parent == cp)
            c->parent = pp ? pp : (init_proc ? init_proc->proc : NULL);
    }
    return woke;
}

/* The last act of a task: record its code and give the slot back as one
 * step.  Two separate steps left a window in which the slot was neither
 * waiting-for-us nor free, and a parent that scanned there would block
 * forever on a result that had already been recorded. */
static void retire_and_reap(thread_t *p, int code)
{
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    thread_t *woke;

#ifdef TOS_KTHREADS
    /* A process leader takes its threads with it: they share the window that
     * is about to disappear. */
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].leader == p)
            proc_retire_locked(&threads[i]);
#endif
    woke = reap_into_parent_locked(p, code);
    proc_retire_locked(p);
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    if (woke)
        preempt_if_higher(woke);
}

/* SIGKILL against a task that is not running.  Called with proc_lock held.
 *
 * task_release() closes descriptors and may take proc_lock itself (pipe
 * wakeups), so it cannot run inside this critical section.  Mark the slot
 * ST_KILLING first: it is no longer schedulable and proc_alloc() only reuses
 * ST_FREE, so the pointer stays valid while the caller drops the lock and
 * releases the files.  Returns 1 when the caller must call
 * task_kill_finish(); 0 means the kthread path deferred the work. */
static int task_kill_prepare_locked(thread_t *p, int code)
{
#ifdef TOS_KTHREADS
    /* A thread dies at its own trap exit so the live_threads accounting and
     * the group finalisation run in one place; a leader with live threads
     * follows the same deferred path process_exit() uses. */
    if (p->leader || (p->proc && p->proc->live_threads > 0)) {
        if (p->leader)
            p->kill_pending = 1;
        else
            begin_group_exit_locked(p, code);
        return 0;
    }
#endif
    proc_set_state(p, ST_KILLING);
    p->cpu = -1;
    return 1;
}

/* Called without proc_lock after task_kill_prepare_locked() returned 1. */
static void task_kill_finish(thread_t *p, int code)
{
    task_release(p);

    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    thread_t *woke = NULL;

    if (p->state == ST_KILLING) {
        woke = reap_into_parent_locked(p, code);
        proc_retire_locked(p);
    }
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    if (woke)
        preempt_if_higher(woke);
}

regs_t *process_exit(regs_t *r, int code)
{
    thread_t *p = current;
    if (!p)
        panic("no current process");

#ifdef TOS_KTHREADS
    /* exit() from a thread ends that thread, not the process: the window and
     * the descriptors are shared with siblings that must keep running. */
    if (p->leader)
        return process_thread_exit(r, code);

    /* A leader whose threads are still alive cannot close the fd table or
     * free the window yet: mark the group dying and let the last thread
     * finish the reap.  This is also the only safe order on SMP, where a
     * sibling may be running on the other core at this instant. */
    if (p->proc && p->proc->live_threads > 0) {
        uint32_t irq = plat_spin_lock_irqsave(&proc_lock);

        begin_group_exit_locked(p, code);
        plat_spin_unlock_irqrestore(&proc_lock, irq);

        mailbox_drop(thread_pid(p));
        return sched_switch(r);
    }
#endif

    task_release(p);

    /* Nothing can read messages addressed to a task that is leaving; give
     * their slots back now instead of waiting for the pool to fill up. */
    mailbox_drop(thread_pid(p));

    int is_init = (p == init_proc);
    if (is_init) {
        /* Raise the flag *before* the slot goes back to the table: a CPU
         * that sees an empty run queue must be able to see it, and the only
         * way to guarantee that without a shared clock is to make the flag
         * visible first.  Folding the reap in lets it share the lock, and
         * keeps the flag ahead of the slot as before. */
        uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
        thread_t *woke;

        sys_down = 1;
#ifdef TOS_KTHREADS
        for (int i = 0; i < MAX_THREADS; i++)
            if (threads[i].leader == p)
                proc_retire_locked(&threads[i]);
#endif
        woke = reap_into_parent_locked(p, code);
        proc_retire_locked(p);
        plat_spin_unlock_irqrestore(&proc_lock, irq);

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
    thread_t *p = current;
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    int code;

    if (!p || !p->proc) {
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return done(r, -1);
    }

    /* The scan and the sleep are one critical section.  A child exiting on
     * the other CPU does reap + retire under this same lock, so there are
     * only two orders and both are correct:
     *
     *   it exits first  -> the slot is already ST_FREE, the loop skips it,
     *                      and the code it recorded is taken below;
     *   we scan first   -> reap finds this thread in ST_WAIT_CHILD and hands
     *                      the code straight into its saved frame.
     *
     * The old code scanned outside the lock and slept afterwards, which left
     * a third order: recorded, then we sleep, then nobody ever wakes us. */
    /* A child that has already gone is in the lag list, and taking it first
     * is what keeps the codes straight: several children can exit between
     * two wait() calls, and the one asked about is not necessarily the last
     * to go. */
    if (lag_take(p->proc, pid, &code)) {
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return done(r, code);
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *c = &threads[i];

        /* One row per process: the main thread carries the id a child is
         * waited for, its workers are the same child.  A kernel thread has
         * no process and can never be a child. */
        if (c->state == ST_FREE || c->leader || !c->proc)
            continue;
        if (c->proc->parent != p->proc)
            continue;
        if (pid >= 0 && c->proc->pid != pid)
            continue;
        p->wait_pid = pid;
        return block_locked(r, ST_WAIT_CHILD, irq);
    }

    /* No live child by that pid, and no remembered code either: it never
       existed (or it is one of the exits that did not fit in lag_pid). */
    plat_spin_unlock_irqrestore(&proc_lock, irq);
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
    uint32_t irq;
    thread_t *victim;

    if (sig != SIGKILL && sig != SIGSTOP && sig != SIGCONT)
        return -1;

    /* Resolve and mutate under the same lock the scheduler, exit and reap
     * paths use.  proc_by_pid() used to run unlocked, so another CPU could
     * retire or reuse the slot between the scan and every field write below:
     * a SIGCONT could turn an ST_NEW exec (no window yet) into ST_READY, and
     * a ^Z could be overwritten by that CPU's sched_switch(). */
    irq = plat_spin_lock_irqsave(&proc_lock);
    victim = proc_by_pid(pid);
    if (!victim || victim == init_proc) {
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return -1;
    }

#ifdef TOS_KTHREADS
    /* A signal aimed at a thread acts on its group - that is what ^C and a
     * shell `kill <pid>` mean for a multithreaded program.  The exception is
     * the caller's own group: retiring the slot this CPU is running would
     * leave the scheduler with a task that no longer exists. */
    if (victim->leader && PROC_GROUP(current) != victim->leader)
        victim = victim->leader;
#endif

    /* SIGSTOP and SIGCONT park or release a whole process.  A kernel thread
     * has no process to park, so only SIGKILL applies to it. */
    if (sig != SIGKILL && !victim->proc) {
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return -1;
    }

    /* "Same as current" only means "on this CPU" while it is actually
     * running.  sched_switch() idles without clearing `current`, so a task
     * asleep with the CPU parked beside it is still `current` - and taking
     * the deferred branch there put the SIGKILL in kill_pending, to be acted
     * on at a trap exit the sleeper would not reach for another three
     * seconds.  Measured: ^C on a foreground `sleep 3000` did nothing until
     * the nap was over.  A blocked task has no live frame to protect; block()
     * copied its registers into ->saved before switching away. */
    if (victim == current && victim->state == ST_RUNNING && sig == SIGKILL) {
        victim->kill_pending = 1;
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return 0;
    }

    if (sig == SIGKILL) {
        int was_current = (victim == current);

        /* A victim running on the other CPU must not be ripped out of its
         * frame from here: its own trap exit is the only safe boundary.  The
         * tick makes that happen within a millisecond. */
        if (victim->state == ST_RUNNING) {
            victim->kill_pending = 1;
            plat_spin_unlock_irqrestore(&proc_lock, irq);
            kprintf("\n[pid %d] killed\n", pid);
            return 0;
        }

        if (victim->state == ST_KILLING) {
            plat_spin_unlock_irqrestore(&proc_lock, irq);
            return -1;
        }

        int immediate = task_kill_prepare_locked(victim, 137);
        plat_spin_unlock_irqrestore(&proc_lock, irq);

        /* Release descriptors and retire outside the table lock: fd_close_in
         * takes it for pipe wakeups.  ST_KILLING kept the slot unreusable in
         * between. */
        if (immediate)
            task_kill_finish(victim, 137);

        /* The CPU was only parked next to it, not inside it: stop pointing
           at a slot that is now free, the way process_exit() does. */
        if (was_current)
            current = NULL;
        kprintf("\n[pid %d] killed\n", pid);
        return 0;
    }

    if (sig == SIGSTOP) {
        int stopped = 0;

        /* The flag belongs to the process: a thread on the CPU parks at its
         * next trap exit (process_suspend), the ones waiting in the run
         * queue park now, and a blocked thread is already off the CPU and
         * parks when it wakes.  SIGCONT is what clears the flag again. */
        victim->proc->stop_pending = 1;
        for (int i = 0; i < MAX_THREADS; i++) {
            thread_t *q = &threads[i];

            if (q->proc != victim->proc || q->state != ST_READY)
                continue;
            proc_set_state(q, ST_STOPPED);
            q->cpu = -1;
            stopped = 1;
        }
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        if (stopped)
            kprintf("\n[pid %d] stopped\n", pid);
    } else { /* SIGCONT */
        int resumed = 0;

        victim->proc->stop_pending = 0;
        for (int i = 0; i < MAX_THREADS; i++) {
            thread_t *q = &threads[i];

            if (q->proc != victim->proc || q->state != ST_STOPPED)
                continue;
            proc_set_state(q, ST_READY);
            q->cpu = -1;
            preempt_if_higher(q);
            resumed = 1;
        }
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        if (resumed)
            kprintf("[pid %d] continued\n", pid);
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

#if !defined(__riscv) && !defined(__arm__)
#error "TOS_KTHREADS is implemented for RV32 and ARM: it needs plat_frame_set_arg() and a preemptive trap frame.  See docs/REAL-THREADS.md."
#endif

/* User pointers are checked against the *group* window: a thread slot has no
 * window of its own, so process_user_ok(current, ...) would reject every
 * address the thread legitimately owns. */
static int group_user_ok(thread_t *g, uint32_t addr, uint32_t len)
{
    uint32_t end;

    if (len == 0)
        return 1;
    end = addr + len;
    if (end < addr)
        return 0;
    return addr >= PROC_RES(g)->win_base && end <= PROC_RES(g)->win_base + PROC_RES(g)->win_size;
}

/* Called with proc_lock held.  Hands `code` to a sibling blocked in join()
 * and makes it runnable; returns it so the caller can preempt outside the
 * lock. */
static thread_t *wake_joiner_locked(thread_t *tid_task, int code)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *q = &threads[i];

        if (q->state != ST_WAIT_THREAD || q->wait_pid != tid_task->tid)
            continue;
        if (q->wbuf && group_user_ok(PROC_GROUP(q), q->wbuf, sizeof(int)))
            *(int *)q->wbuf = code;
        plat_frame_set_ret(&q->saved, 0);
        proc_set_state(q, ST_READY);
        q->cpu = -1;
        return q;
    }
    return NULL;
}

int process_thread_create(uint32_t entry, uint32_t arg,
                          uint32_t ustack, uint32_t usize)
{
    thread_t *leader = current ? PROC_GROUP(current) : NULL;
    thread_t *p;
    uint32_t low;
    uint32_t irq;
    int tid;

    if (!leader || leader->is_ktask || PROC_RES(leader)->win_size == 0)
        return -1;
    if (entry == 0 || (ustack & 15u) || usize == 0 || (usize & 3u))
        return -1;
    if (ustack <= PROC_RES(leader)->win_base ||
        ustack > PROC_RES(leader)->win_base + PROC_RES(leader)->win_size)
        return -1;
    low = ustack - usize;
    /* The 4 bytes below the stack hold the guard word; the caller promises
     * them (struct tos_thread_stack).  Everything must still be inside the
     * process's window, or the canary write itself would be a wild store. */
    if (low < PROC_RES(leader)->win_base + 8u ||
        low - 4u < PROC_RES(leader)->win_base ||
        low > ustack)
        return -1;

    p = proc_alloc();
    if (!p)
        return -1;

    p->proc = leader->proc;
    p->leader = leader;
    p->prio = current->prio;
    p->stack_top = ustack;
    /* A thread's stack now has a name and a size: the group's protection
     * cannot separate it from the heap around it (both are the same writable
     * data region), so this range is what the software guard checks.  It is
     * not growable - the bytes below belong to the allocator. */
    p->stack_size = usize;
    /* Poison it before the first instruction, like exec() does, so ps's
     * stk/sz column means the same thing for a thread as for a process. */
    memset((void *)low, STACK_POISON, usize);
    /* Guard word just below the stack: stack_guard() reads it on every syscall
     * and a runaway push has to clobber it on the way down. */
    *((uint32_t *)low - 1) = USER_STACK_GUARD;
    /* The heap fields are still copies; nothing reads them through a thread
     * (PROC_OF() is the only accessor, and it goes to the leader), and S3
     * reworks the stack guard before they can go away.  cwd and fds are the
     * group's resource block now, so there is nothing to copy here. */
    p->start_ns = plat_now_ns();

    plat_prepare_user_frame(&p->saved, entry, ustack);
    plat_frame_set_arg(&p->saved, arg);

    irq = plat_spin_lock_irqsave(&proc_lock);
    tid = next_pid++;
    p->tid = tid;
    proc_set_state(p, ST_READY);
    p->cpu = -1;
    leader->proc->live_threads++;
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    return tid;
}

/* Called with proc_lock held.  Closes the group's descriptors (the threads
 * are gone by now) and hands the leader's exit code to its parent. */
static thread_t *finish_group_locked(thread_t *leader)
{
    thread_t *woke;

    /* Zombie thread slots still point at this group and hold a pid; the group
     * is going away, so they go with it (only the leader's window is real). */
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *q = &threads[i];

        if (q != leader && q->leader == leader)
            proc_retire_locked(q);
    }

    task_release(leader);
    woke = reap_into_parent_locked(leader, leader->proc->exit_code);
    proc_retire_locked(leader);
    return woke;
}

/* Called with proc_lock held.  The leader is leaving but threads share its
 * window: mark it dying, forbid scheduling it, and make every sibling exit.
 * A blocked sibling is woken with -1 so it reaches a trap exit; one running
 * on another CPU is killed by kill_pending at its next trap (the tick
 * guarantees that happens within a millisecond).  The last one to leave calls
 * finish_group_locked(). */
static void begin_group_exit_locked(thread_t *leader, int code)
{
    leader->proc->exit_code = code;
    proc_set_state(leader, ST_DYING);
    leader->cpu = -1;

    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *q = &threads[i];

        if (q->leader != leader || q->state == ST_FREE)
            continue;
        q->kill_pending = 1;
        if (q->state == ST_STOPPED) {
            /* A stopped sibling would never reach a trap exit to die at;
             * make it runnable so the group can finish. */
            proc_set_state(q, ST_READY);
            q->cpu = -1;
            continue;
        }
        if (q->state == ST_WAIT_CHILD || q->state == ST_WAIT_INPUT ||
            q->state == ST_WAIT_SLEEP || q->state == ST_WAIT_PIPE ||
            q->state == ST_WAIT_THREAD || q->state == ST_WAIT_MUTEX) {
            plat_frame_set_ret(&q->saved, (uint32_t)-1);
            q->wmutex = NULL;       /* the group (and the word) is going away */
            proc_set_state(q, ST_READY);
            q->cpu = -1;
        }
    }
}

regs_t *process_thread_exit(regs_t *r, int code)
{
    thread_t *p = current;
    thread_t *leader;
    thread_t *woke = NULL;
    thread_t *woke_leader = NULL;
    thread_t *woke_mutex = NULL;
    uint32_t irq;

    if (!p || !p->leader)
        return process_exit(r, code);

    leader = p->leader;
    irq = plat_spin_lock_irqsave(&proc_lock);
    p->exit_code = code;
    /* A thread that dies holding a user mutex (the heap lock is the one this
     * exists for) must hand it on: a fault inside malloc would otherwise
     * leave every sibling waiting forever.  When the whole group is dying
     * there is nobody to hand it to. */
    if (p->held_mutex) {
        if (leader->state != ST_DYING)
            woke_mutex = mutex_handoff_locked(p->held_mutex, p);
        p->held_mutex = NULL;
    }
    p->wmutex = NULL;
    woke = wake_joiner_locked(p, code);
    if (woke) {
        /* The waiter has the code in its frame and its user buffer, so the
         * slot can go right away. */
        proc_retire_locked(p);
    } else {
        /* No joiner yet: keep the slot (and the code) until join() takes it,
         * or until the leader exits and retires the whole group.  A thread
         * nobody ever joins therefore holds one table slot - the same cost a
         * zombie child has in POSIX until wait(). */
        proc_set_state(p, ST_THREAD_ZOMBIE);
        p->cpu = -1;
    }

    if (leader->proc->live_threads > 0)
        leader->proc->live_threads--;
    if (leader->state == ST_DYING && leader->proc->live_threads == 0)
        woke_leader = finish_group_locked(leader);

    plat_spin_unlock_irqrestore(&proc_lock, irq);

    if (woke_leader)
        preempt_if_higher(woke_leader);
    if (woke_mutex)
        preempt_if_higher(woke_mutex);
    if (woke)
        preempt_if_higher(woke);
    return sched_switch(r);
}

regs_t *process_thread_join(regs_t *r, int tid, uint32_t codeptr)
{
    thread_t *me = current;
    thread_t *g = me ? PROC_GROUP(me) : NULL;
    thread_t *t = NULL;
    uint32_t irq;
    int code;

    if (!g || (codeptr && !group_user_ok(g, codeptr, sizeof(int))))
        return done(r, -1);

    irq = plat_spin_lock_irqsave(&proc_lock);

    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].state != ST_FREE && threads[i].tid == tid) {
            t = &threads[i];
            break;
        }
    if (!t || t->leader != g || t == me) {
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return done(r, -1);
    }

    if (t->state == ST_THREAD_ZOMBIE) {
        code = t->exit_code;
        proc_retire_locked(t);
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        if (codeptr)
            *(int *)codeptr = code;
        return done(r, 0);
    }

    me->wait_pid = tid;
    me->wbuf = codeptr;
    return block_locked(r, ST_WAIT_THREAD, irq);
}

#endif /* TOS_KTHREADS */

/* ---- misc ------------------------------------------------------------ */

#ifdef TOS_KTHREADS
/* One user-space mutex word at a time, across all processes.  The kernel is
 * where a test-and-set can be atomic on every port: user mode has no atomics
 * on the M0+ or the C3, and on a uniprocessor the trap (interrupts masked) is
 * what excludes preemption.  On SMP the guard lock serialises the update, and
 * its acquire/release barriers order the user's critical-section accesses
 * around the word.  Critical sections are a few instructions (the heap free
 * list), so one guard for the whole system is enough. */
static spin_t user_mutex_guard = SPIN_INIT;

/* Give `word` to the first thread sleeping on it, or release it.  Called
 * with proc_lock held.  Ownership moves with the word (it stays 1), so the
 * woken thread returns from kmutex_lock() already holding it - no race with
 * a third thread that would otherwise grab it between wake and retry. */
static thread_t *mutex_handoff_locked(volatile int *word, const thread_t *dead)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *q = &threads[i];

        if (q == dead || q->wmutex != word || q->state != ST_WAIT_MUTEX)
            continue;
        q->wmutex = NULL;
        q->held_mutex = word;
        plat_frame_set_ret(&q->saved, 0);
        proc_set_state(q, ST_READY);
        q->cpu = -1;
        return q;
    }
    *word = 0;
    return NULL;
}

int process_mutex_try(uint32_t ptr)
{
    thread_t *p = current;
    volatile int *word;
    uint32_t irq;
    int got = 0;

    if (!p || (ptr & 3u) || !process_user_ok(ptr, sizeof(int)))
        return -1;
    irq = plat_spin_lock_irqsave(&proc_lock);
    word = (volatile int *)ptr;
    if (*word == 0) {
        *word = 1;
        p->held_mutex = word;
        got = 1;
    }
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    return got;
}

/* Blocking lock.  The check and the sleep are one critical section, so an
 * unlock between them cannot be lost (same shape as block_locked's pipe
 * waits).  The unlocker sets our return value to 0 and hands the word over;
 * if nobody unlocks we stay here. */
regs_t *process_mutex_lock(regs_t *r, uint32_t ptr)
{
    thread_t *p = current;
    volatile int *word;
    uint32_t irq;

    if (!p || (ptr & 3u) || !process_user_ok(ptr, sizeof(int)))
        return done(r, -1);
    word = (volatile int *)ptr;

    irq = plat_spin_lock_irqsave(&proc_lock);
    if (*word == 0) {
        *word = 1;
        p->held_mutex = word;
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        return done(r, 0);
    }
    p->wmutex = word;
    return block_locked(r, ST_WAIT_MUTEX, irq);
}

int process_mutex_unlock(uint32_t ptr)
{
    thread_t *p = current;
    thread_t *woke;
    uint32_t irq;

    if (!p || (ptr & 3u) || !process_user_ok(ptr, sizeof(int)))
        return -1;
    irq = plat_spin_lock_irqsave(&proc_lock);
    if (p->held_mutex == (volatile int *)ptr)
        p->held_mutex = NULL;
    woke = mutex_handoff_locked((volatile int *)ptr, p);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
    if (woke)
        preempt_if_higher(woke);
    return 0;
}

int process_schedstat(uint32_t ptr)
{
    struct { uint32_t sw, self, ticks, prot; } st;

    if (!process_user_ok(ptr, sizeof(st)))
        return -1;
    st.sw = sched_switches;
    st.self = sched_self;
    st.ticks = sched_ticks;
    st.prot = sched_prot;
    memcpy((void *)ptr, &st, sizeof(st));
    return 0;
}
#endif

uint32_t process_sbrk(uint32_t bytes)
{
    thread_t *g;
    uint32_t old, newbrk;

    if (!current)
        return 0;
    g = PROC_GROUP(current);

    /* Threads share the leader's break, and two of them can call sbrk() on
     * two CPUs at once.  The read-modify-write below has to be one step, or
     * both calls return the same block and one allocation is lost (measured
     * on rv32smp: 400 sbrk(16) calls from four threads moved the break by
     * 25,088..25,264 bytes instead of 25,600).  This is syscall context, so
     * interrupts are already masked; the plain group lock is what the
     * descriptor table uses too. */
    group_lock(current);
    old = PROC_RES(g)->brk;
    if (bytes != 0) {
        newbrk = old + bytes;
        if (newbrk < PROC_RES(g)->heap_base || newbrk > PROC_RES(g)->heap_end) {
            group_unlock(current);
            return 0;
        }
        PROC_RES(g)->brk = newbrk;
    }
    group_unlock(current);

    if (bytes != 0)
        /* The heap bound is part of the protection: without this the new
         * pages would stay unreachable for the task that just asked for
         * them.  Outside the lock on purpose: apply_protection() reads the
         * group's current break, so a sibling that moved it further only
         * widens this CPU's regions. */
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
 *     task share the machine.  A TOR PMP (the C3) has no alignment rule, so
 *     a board that says so can double any window's stack, however its top
 *     happens to be aligned - which is why the C3 can also keep 4 KiB
 *     windows without losing this.
 *   - the heap must not be in the way: brk + guard has to stay below the new
 *     low, so the hole that turns the *next* overflow into a fault instead of
 *     a silent overwrite keeps its board size.
 *
 * Returning 1 means the faulting instruction can be re-executed unchanged:
 * it was refused before it wrote anything.  0 leaves the decision to the
 * caller, which kills the task exactly as it always did. */
/* Called on every syscall from user mode when the board has no faulting
 * protection unit.  1 = the stack is intact (or was grown); 0 = the task has
 * run out and could not be grown, and the caller must kill it. */
int process_stack_guard(const regs_t *r)
{
    thread_t *p = current;
    uint32_t low, sp, *gw;

    if (!p || !p->proc || p->proc->win_size == 0 || p->stack_size == 0)
        return 1;
    /* A process stack is covered by the port's own mechanism when it has
     * one (an MPU/PMP rule plus, on some boards, the same canary).  A thread
     * stack is inside the group's writable data region, where no MPU/PMP rule
     * can separate it from the heap, so its canary is the only check and it
     * runs on every port. */
    if (!PROC_IS_THREAD(p) && !plat_user_stack_guard())
        return 1;

    low = p->stack_top - p->stack_size;
    sp = r ? plat_frame_get_user_sp(r) : low;
    gw = (uint32_t *)low - 1;

    /* Two independent witnesses: the sp this call was made with (exact, and
     * the usual case - a recursion that ran out then called something), and
     * the canary (a write past the bottom without sp following it).  `<=`
     * on the top: a fresh thread starts with sp == stack_top, and its first
     * syscall is legitimate - the exception frame is below the pointer. */
    if (sp >= low && sp <= p->stack_top && *gw == USER_STACK_GUARD)
        return 1;

    kprintf("\n[pid %d] %s overflow (sp 0x%08x, stack 0x%08x..0x%08x)\n",
            thread_pid(p), PROC_IS_THREAD(p) ? "thread stack" : "stack",
            sp, low, p->stack_top);
    *gw = USER_STACK_GUARD;          /* re-arm before the growth decision */
    if (!process_stack_grow(sp < low ? sp : low - 4u, sp)) {
        kprintf("[pid %d] %s; killing\n", thread_pid(p),
                PROC_IS_THREAD(p) ? "a thread stack cannot grow in place"
                                  : "no room to grow it");
        return 0;
    }
    return 1;
}

/* True when sp is outside the task's current stack, i.e. a load/store fault
 * is a stack-overflow candidate rather than a wild pointer inside the stack.
 * Used by fault-driven ports after process_stack_grow() refuses. */
int process_stack_overflow_at(uint32_t sp)
{
    thread_t *p = current;

    if (!p || !p->proc || p->proc->win_size == 0 || p->stack_size == 0)
        return 0;
    return sp < p->stack_top - p->stack_size;
}

int process_stack_grow(uint32_t addr, uint32_t sp)
{
    thread_t *p = current;
    uint32_t old, want, low, poison_end;

    if (!p || !p->proc || p->proc->win_size == 0 || p->stack_size == 0)
        return 0;
#ifdef TOS_KTHREADS
    /* A thread stack is a block inside the process heap: the bytes below it
     * are another allocation, not a hole the stack may take.  Overflow is
     * therefore fatal for the thread, and stack_guard() says so. */
    if (p->leader)
        return 0;
#endif

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

    if (plat_window_needs_stack_alignment() && (p->stack_top % want) != 0)
        return 0;                            /* the port cannot describe it */

    low = p->stack_top - want;
    if (addr < low || PROC_RES(p)->brk + HEAP_GUARD > low) {
        /* Refused for want of room, not because the program is buggy: say
         * which knob turns, since the fault message alone only says "access
         * fault".  addr < low means the write jumped further than one step;
         * brk in the way means the heap got the slack first. */
        kprintf("\n[pid %d] %u KiB stack and no room to double it; "
                "declare TOS_STACK() for the depth you need\n",
                thread_pid(p), old / 1024u);
        return 0;
    }

    /* Poison the new strip before it becomes writable: stack_used() scans up
     * from the bottom and would otherwise report the heap's leftovers as
     * stack the program had used.
     *
     * A board with a software stack guard reaches here with sp ALREADY below
     * the old stack_low: the live frames between sp and old_stack_low were
     * written before the growth and must survive.  Poisoning all of them
     * turns the next function return into a jump to 0xcccccccc (measured on
     * the RP2350 M33: deep 3 printed "stack grown to 8 KiB" and immediately
     * HardFaulted).  Fault-driven growth never has this problem: the refused
     * store left nothing in that strip, and sp is still inside the old stack,
     * so poison_end stays at old_stack_low and the behaviour is unchanged. */
    poison_end = p->stack_top - old;
    if (sp >= low && sp < poison_end)
        poison_end = sp;
    memset((void *)low, STACK_POISON, poison_end - low);

    p->stack_size = want;
    PROC_RES(p)->heap_end = low - HEAP_GUARD;
    if (plat_user_stack_guard())
        *((uint32_t *)low - 1) = USER_STACK_GUARD;
    apply_protection(p);

    kprintf("\n[pid %d] stack grown to %u KiB, heap ceiling %u KiB\n",
            thread_pid(p), want / 1024u,
            (PROC_RES(p)->heap_end - PROC_RES(p)->heap_base) / 1024u);
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
#ifdef TOS_KTHREADS
    case ST_WAIT_THREAD:   return "join ";
    case ST_WAIT_MUTEX:    return "mutex";
    case ST_THREAD_ZOMBIE: return "zombi";
    case ST_DYING:         return "dying";
#endif
    case ST_KILLING:    return "kill ";
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
static uint32_t stack_used(const thread_t *p)
{
    const uint8_t *low;
    uint32_t i = 0;

    if (!p->proc || p->proc->win_size == 0 || p->stack_top == 0 ||
        p->stack_size == 0)
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

    n = ksnprintf(buf, cap,
                  " pid  tid ppid prio state  c cpu%% cpums wink stk/sz  name\n");
    if (n < 0)
        return 0;
    off = (uint32_t)n;

    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *p = &threads[i];
        char cpubuf[4];
        uint32_t age, busy, pct10, stk10;

        if (p->state == ST_FREE || !p->proc || p->is_ktask)
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
                      "%4d %4d %4d %4d %s %2s %4u.%u %5u %4u %4u.%u/%u  %s\n",
                      p->proc->pid, p->tid,
                      p->proc->parent ? p->proc->parent->pid : 0, p->prio,
                      state_name(p->state), cpubuf, pct10 / 10u, pct10 % 10u,
                      busy / 1000u, p->proc->win_size / 1024u,
                      stk10 / 10u, stk10 % 10u, p->stack_size / 1024u,
                      p->proc->name);
        if (n > 0)
            off += (uint32_t)n;
    }
    return off;
}

/* One line per thread, for scripts and for /proc/threads: the tuple
 * (tgid, tid) identifies a thread, the way Linux's /proc/<pid>/task does.
 * ps above is the human view - same rows, with the process columns repeated
 * so a thread can be read without looking at its leader.  Kernel threads are
 * not here; /proc/ktask is their table. */
uint32_t process_threads_text(char *buf, uint32_t cap)
{
    uint32_t off = 0;
    int n = ksnprintf(buf, cap, " tgid  tid state  c prio stk/sz  name\n");

    if (n < 0)
        return 0;
    off = (uint32_t)n;

    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *p = &threads[i];
        char cpubuf[3];
        uint32_t stk10;

        if (p->state == ST_FREE || !p->proc || p->is_ktask)
            continue;
        /* A character, like ps: the formatter pads a negative %d after the
         * sign, so "-1" would come out as "- 1" and shift the row. */
        cpubuf[0] = p->cpu < 0 ? '-' : (char)('0' + (p->cpu % 10));
        cpubuf[1] = '\0';
        stk10 = stack_used(p) * 10u / 1024u;
        n = ksnprintf(buf + off, cap - off,
                      "%5d %4d %s %2s %4d %4u.%u/%u  %s\n",
                      p->proc->pid, p->tid, state_name(p->state),
                      cpubuf, p->prio, stk10 / 10u, stk10 % 10u,
                      p->stack_size / 1024u, p->proc->name);
        if (n < 0)
            break;
        off += (uint32_t)n;
        if (off >= cap - 1)
            break;
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

    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].state != ST_FREE)
            count++;
    return count;
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
static thread_t *pipe_deliver_locked(struct pipe *p, int dir)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *w = &threads[i];
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
        proc_set_state(w, ST_READY);
        return w;                   /* n == 0 is EOF and is delivered too */
    }
    return NULL;
}

/* Every waiter that can be satisfied right now.  Returns the one with the
 * best priority, for the caller's post-unlock preempt_if_higher(). */
static thread_t *pipe_wake_locked(struct pipe *p, int dir)
{
    thread_t *best = NULL;
    thread_t *w;

    while ((w = pipe_deliver_locked(p, dir)) != NULL)
        if (!best || w->prio < best->prio)
            best = w;
    return best;
}

static regs_t *block_on_pipe_locked(regs_t *r, struct pipe *p, int dir,
                                    uint32_t buf, uint32_t len, uint32_t irq)
{
    thread_t *me = current;

    me->saved = *r;
    plat_frame_set_ret(&me->saved, (uint32_t)-1);
    me->wpipe = p;
    me->wdir  = dir;
    me->wbuf  = buf;
    me->wlen  = len;
    proc_set_state(me, ST_WAIT_PIPE);
    me->cpu   = -1;
    plat_spin_unlock_irqrestore(&proc_lock, irq);

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
    if (!tty_line_ready()) {
        return process_block_read(r, ptr, room);
    }
    return done(r, (uint32_t)tty_take_line((char *)ptr, (int)room));
}

regs_t *process_read(regs_t *r, int fd, uint32_t ubuf, uint32_t len)
{
    thread_t *me = current;
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
        if (!f->u.chr.dev->ops->read)
            return done(r, (uint32_t)-1);
        n = f->u.chr.dev->ops->read(f->u.chr.devpriv, f->pos, (void *)ubuf, len);
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
        uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
        thread_t *woke;
        int n;

        n = pipe_read(f->u.pipe, (void *)ubuf, len);
        if (n == PIPE_EAGAIN)
            return block_on_pipe_locked(r, f->u.pipe, 0, ubuf, len, irq);
        /* Room appeared: writers may be parked waiting for it. */
        woke = n > 0 ? pipe_wake_locked(f->u.pipe, 1) : NULL;
        plat_spin_unlock_irqrestore(&proc_lock, irq);
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
    thread_t *me = current;
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
        /* A short count means "the sink is behind, ask again", not an error:
         * the USB CDC mirror drops when its ring is full, so console_write_some()
         * stops there and lets the caller yield - the deferred pump only runs
         * in Thread mode, which this syscall is not in.  write_all() in the
         * user library retries on a short count and gives up only on a
         * negative one; a host that never drains is handled inside by a
         * quarter-second cut-off after which the block is written anyway. */
        return done(r, (uint32_t)console_write_some((const char *)ubuf, len));

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

        if (!f->u.chr.dev->ops->write)
            return done(r, (uint32_t)-1);
        n = f->u.chr.dev->ops->write(f->u.chr.devpriv, f->pos, (const void *)ubuf, len);
        if (n > 0)
            f->pos += (uint32_t)n;
        return done(r, (uint32_t)n);
    }

    case FD_PIPE_W: {
        uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
        thread_t *woke;
        int n;

        n = pipe_write(f->u.pipe, (const void *)ubuf, len);
        if (n == PIPE_EAGAIN)
            return block_on_pipe_locked(r, f->u.pipe, 1, ubuf, len, irq);
        /* Data for a sleeping reader - delivered before the lock goes, so
         * nobody can drain it in between and leave the reader parked. */
        woke = n > 0 ? pipe_wake_locked(f->u.pipe, 0) : NULL;
        plat_spin_unlock_irqrestore(&proc_lock, irq);
        if (woke)
            preempt_if_higher(woke);
        return done(r, (uint32_t)(n == PIPE_EPIPE ? -1 : n));
    }

    default:
        return done(r, (uint32_t)-1);
    }
}

/* ---- descriptor table seam (kernel/fd.c) ------------------------------
 *
 * The table is a field of struct proc, and a pipe that loses an end has to
 * wake whoever sleeps on the other side: two things only this file may
 * touch.  fd.c owns the table operations and calls these small hooks
 * instead (docs/ARCHITECTURE-REVIEW-2026-11-22.md, P0-2).
 */
file_t *proc_fd_table(thread_t *t)
{
    return t->proc ? t->proc->fds : NULL;
}

void proc_fd_lock(thread_t *t)   { group_lock(t); }
void proc_fd_unlock(thread_t *t) { group_unlock(t); }

/* The last reader (write_end == 0) or writer (1) of `p` went away: wake the
 * other side so it sees EOF or EPIPE, and free the pipe when this was the
 * last reference of any kind.  Split out of fd_close_in() because waking a
 * task is this file's business. */
void process_pipe_closed(struct pipe *p, int write_end)
{
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    thread_t *woke = NULL;
    int last;

    if (write_end) {
        last = pipe_drop_writer(p);
        if (!last && pipe_writers(p) == 0)
            woke = pipe_wake_locked(p, 0);      /* readers must see EOF */
    } else {
        last = pipe_drop_reader(p);
        if (!last && pipe_readers(p) == 0)
            woke = pipe_wake_locked(p, 1);      /* writers have nobody left */
    }
    plat_spin_unlock_irqrestore(&proc_lock, irq);

    if (last)
        kfree(p);               /* no references left at all */
    if (woke)
        preempt_if_higher(woke);
}

/* Readiness of a pipe end for poll(); same lock the ring is documented to
 * use (pipe.h). */
int process_pipe_ready(struct pipe *p, int write_end)
{
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    int ready = write_end ? pipe_writable(p) : pipe_readable(p);

    plat_spin_unlock_irqrestore(&proc_lock, irq);
    return ready;
}

void process_start_shell(const char *path)
{
    /* Published already claimed: see proc_create_claim().  On an SMP build
     * the other core's idle loop is scanning for READY tasks from the moment
     * cpu_init() returns, and the shell must not be one of them. */
    thread_t *p = proc_create_claim(path, NULL, 0, 1);
    if (!p)
        panic("cannot load initial program '%s'", path);

    init_proc = p;

    plat_set_kernel_stack(KSTACK_TOP);
    apply_protection(p);

    /* Straight into the frame proc_create prepared. */
    plat_enter_user(&p->saved);
}
