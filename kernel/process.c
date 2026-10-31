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
    uint32_t cr3;
    uint32_t entry;
    uint32_t user_esp;
    uint32_t brk;
    uint32_t brk_end;
    struct proc *parent;
    regs_t saved;
    file_t fds[MAX_FDS];
} proc_t;

static proc_t procs[MAX_PROCS];
static proc_t *current;
static int next_pid = 1;

static uint8_t kernel_stack[16384] __attribute__((aligned(16)));
#define KSTACK_TOP ((uint32_t)kernel_stack + sizeof(kernel_stack))



void process_init(void)
{
    memset(procs, 0, sizeof(procs));
    current = NULL;
    next_pid = 1;
    plat_set_kernel_stack(KSTACK_TOP);
}

int process_current_pid(void)
{
    return current ? current->pid : 0;
}

uint32_t process_current_cr3(void)
{
    return current ? current->cr3 : 0;
}

static proc_t *proc_alloc(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!procs[i].used)
            return &procs[i];
    }
    return NULL;
}

static uint32_t build_initial_stack(uint32_t pd, char *const argv[], int argc)
{
    if (argc > MAX_ARGS)
        return 0;

    for (int i = 0; i < STACK_PAGES; i++) {
        uint32_t v = PLAT_APP_STACK_TOP - (uint32_t)(i + 1) * PLAT_PAGE_SIZE;
        if (plat_user_page(pd, v) != 0)
            return 0;
    }

    uint32_t stack_low = PLAT_APP_STACK_TOP - STACK_PAGES * PLAT_PAGE_SIZE;
    /* Leave a little headroom at the very top: some programs treat argv[]
       as writable storage and grow it upwards through the initial frame. */
    uint32_t sp = PLAT_APP_STACK_TOP - 256;
    uint32_t uargv[MAX_ARGS];

    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        if (sp < stack_low + 64 + len)
            return 0;
        sp -= len;
        plat_write_user(pd, sp, argv[i], len);
        uargv[i] = sp;
    }

    sp &= ~0xFu;
    sp -= (uint32_t)(argc + 1) * 4;
    if (sp < stack_low)
        return 0;
    for (int i = 0; i < argc; i++)
        plat_write_user(pd, sp + (uint32_t)i * 4, &uargv[i], 4);
    uint32_t zero = 0;
    plat_write_user(pd, sp + (uint32_t)argc * 4, &zero, 4);

    sp -= 4;
    if (sp < stack_low)
        return 0;
    plat_write_user(pd, sp, (uint32_t[]){ (uint32_t)argc }, 4);

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

    uint32_t pd = plat_as_create();
    if (!pd) {
        ramdisk_release(data);
        return NULL;
    }

    /* Loader writes through the process's own page directory, and relocates
       position independent (ET_DYN) images to PLAT_APP_BASE. */
    elf_set_address_space(pd);

    uint32_t entry = 0;
    if (elf_load(data, size, PLAT_APP_BASE, &entry) != 0) {
        kprintf("exec: '%s' is not a valid ELF32 program\n", path);
        elf_set_address_space(0);
        plat_as_destroy(pd);
        ramdisk_release(data);
        return NULL;
    }
    elf_set_address_space(0);
    ramdisk_release(data);

    uint32_t sp = build_initial_stack(pd, argv, argc);
    if (!sp) {
        kprintf("exec: out of memory\n");
        plat_as_destroy(pd);
        return NULL;
    }

    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->pid = next_pid++;
    p->cr3 = pd;
    p->entry = entry;
    p->user_esp = sp;
    p->brk = PLAT_APP_HEAP_BASE;
    p->brk_end = PLAT_APP_HEAP_BASE;
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
    plat_activate_as(p->cr3);
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

    plat_as_destroy(p->cr3);

    /* close any files the process still had open */
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i].used)
            ramdisk_close(p->fds[i].fh);
    }

    p->used = 0;

    current = parent;
    *r = parent->saved;
    r->eax = (uint32_t)code;
    plat_activate_as(parent->cr3);
}

uint32_t process_sbrk(uint32_t bytes)
{
    if (!current)
        return 0;

    uint32_t old = current->brk;
    if (bytes == 0)
        return old;

    uint32_t newbrk = old + bytes;
    if (newbrk < PLAT_APP_HEAP_BASE || newbrk > PLAT_APP_MAX)
        return 0;

    while (current->brk_end < newbrk) {
        if (plat_user_page(current->cr3, current->brk_end) != 0)
            return 0;
        current->brk_end += PLAT_PAGE_SIZE;
    }

    current->brk = newbrk;
    plat_flush_tlb(old & ~0xFFFu);
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
    plat_activate_as(p->cr3);

    regs_t frame;
    plat_prepare_user_frame(&frame, p->entry, p->user_esp);

    plat_enter_user(&frame);
}