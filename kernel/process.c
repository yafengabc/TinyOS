#include "process.h"
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

typedef struct {
    int      used;
    int      fh;           /* littlefs open-file handle, -1 when unused */
    uint32_t size;
    uint32_t pos;
} file_t;

typedef struct proc {
    int used;
    int pid;
    /* This task's slice of the flat address space: image at the bottom,
     * sbrk heap above it, stack at the top. */
    uint32_t win_base;
    uint32_t stack_top;
    uint32_t heap_base;
    uint32_t heap_end;
    uint32_t entry;
    uint32_t user_esp;
    uint32_t brk;
    struct proc *parent;
    regs_t saved;
    file_t fds[MAX_FDS];
} proc_t;

static proc_t procs[MAX_PROCS];
static proc_t *current;
static int next_pid = 1;

/* No paging: task windows come from a bump pointer inside
 * [PLAT_APP_BASE, PLAT_APP_MAX).  exec nests strictly LIFO (a parent blocks
 * until its child exits), so resetting the pointer on exit reclaims the
 * window without needing a free list. */
static uint32_t app_next;

/* One kernel stack only: the 16 KiB stack entry.S switches to before kmain
 * is the same stack traps from user mode land on (via the TSS esp0).  The
 * boot-time frames are long gone by the time the first user trap arrives,
 * so they never collide.  Keeps 16 KiB of .bss for the heap. */
extern uint8_t __kernel_stack_top[];
#define KSTACK_TOP ((uint32_t)__kernel_stack_top)

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

void process_init(void)
{
    memset(procs, 0, sizeof(procs));
    current = NULL;
    next_pid = 1;
    app_next = PLAT_APP_BASE;
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

static proc_t *proc_alloc(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!procs[i].used)
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

    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        if (sp < stack_low + 64 + len)
            return 0;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        uargv[i] = sp;
    }

    sp &= ~0xFu;
    sp -= (uint32_t)(argc + 1) * 4;
    if (sp < stack_low)
        return 0;
    for (int i = 0; i < argc; i++)
        memcpy((void *)(sp + (uint32_t)i * 4), &uargv[i], 4);
    uint32_t zero = 0;
    memcpy((void *)(sp + (uint32_t)argc * 4), &zero, 4);

    sp -= 4;
    if (sp < stack_low)
        return 0;
    uint32_t argc_word = (uint32_t)argc;
    memcpy((void *)sp, &argc_word, 4);

    return sp;
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
    if (app_next + PLAT_APP_WINDOW > PLAT_APP_MAX) {
        kprintf("exec: out of task windows\n");
        ramdisk_release(data);
        return NULL;
    }

    uint32_t base = app_next;

    /* The loader relocates the position independent (ET_DYN) image to
       `base` and checks every write against the window. */
    elf_set_window(base, base + PLAT_APP_WINDOW);

    uint32_t entry = 0;
    if (elf_load(data, size, base, &entry) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        elf_set_window(0, 0);
        ramdisk_release(data);
        return NULL;
    }
    elf_set_window(0, 0);
    ramdisk_release(data);

    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->pid = next_pid++;
    p->win_base = base;
    p->stack_top = base + PLAT_APP_WINDOW - 256;
    p->heap_base = base + span;
    p->heap_end = p->stack_top - stack_bytes;
    p->entry = entry;
    p->brk = p->heap_base;

    uint32_t sp = build_initial_stack(p, argv, argc);
    if (!sp) {
        kprintf("exec: out of memory\n");
        p->used = 0;
        return NULL;
    }
    p->user_esp = sp;

    app_next += PLAT_APP_WINDOW;   /* commit: the window is now taken */
    return p;
}

int process_exec(regs_t *r, const char *path, char *const argv[], int argc)
{
    if (!current)
        return -1;

    proc_t *p = proc_create(path, argv, argc);
    if (!p)
        return -1;

    current->saved = *r;
    p->parent = current;

    plat_prepare_user_frame(r, p->entry, p->user_esp);

    current = p;
    plat_set_kernel_stack(KSTACK_TOP);
    return 0;
}

void process_kill_current(regs_t *r, int code)
{
    proc_t *p = current;
    if (!p)
        panic("no current process");

    proc_t *parent = p->parent;
    if (!parent) {
        kprintf("\ninit process exited with code %d - shutting down\n", code);
        plat_shutdown();
    }

    /* close any files the process still had open */
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i].used)
            ramdisk_close(p->fds[i].fh);
    }

    /* Give the window back: children always sit above their parent, so a
       plain pointer reset is enough. */
    app_next = p->win_base;
    p->used = 0;

    current = parent;
    *r = parent->saved;
    plat_frame_set_ret(r, (uint32_t)code);
}

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

    current = p;
    plat_set_kernel_stack(KSTACK_TOP);

    regs_t frame;
    plat_prepare_user_frame(&frame, p->entry, p->user_esp);

    plat_enter_user(&frame);
}
