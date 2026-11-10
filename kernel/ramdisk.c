#include "ramdisk.h"
#include "string.h"
#include "console.h"
#include "cpu.h"
#include "heap.h"
#include "platform.h"
#include "tty.h"
#include "lfs_config.h"   /* must come before lfs.h: defines LFS_NO_* + LFS_MALLOC */
#include "lfs.h"

/* The littlefs partition lives on whatever block device the platform
   provides, starting at PLAT_LFS_LBA: on the PC that is the ATA disk right
   after the kernel image, on qemu-virt it is a RAM disk linked into the
   kernel.  Either way writes go back to the device, so the filesystem is
   genuinely writable. */

#define FS_BLOCK_SIZE  4096
/* The partition's block count is board-specific: PLAT_LFS_BLOCKS (each
 * board's layout.c), so the C3 can size its 4 MiB flash partition up
 * without changing every other board.  FS_BLOCK_SIZE stays shared. */
#define FS_CACHE_SIZE  512
#define MAX_OPEN       8               /* concurrent file handles (== MAX_FDS) */

/* The per-file 512 byte littlefs cache is allocated on open instead of
   living in .bss: an idle handle then costs ~120 bytes, and RAM is paid
   only for files that are actually open.  This keeps MAX_OPEN aligned with
   the per-process fd table without adding 4 KiB of static buffers. */
typedef struct {
    int      used;
    lfs_file_t f;
    struct lfs_file_config fc;   /* points at `cache` under LFS_NO_MALLOC */
    uint8_t *cache;
    uint32_t size;
    /* The path this handle was opened with, so unlink() can refuse to
     * delete a file somebody still holds open (see unlink_locked).  Same
     * length as the resolve_path() buffer in syscall.c, and copied rather
     * than pointed at because that buffer is the caller's. */
    char     path[64];
} ofile_t;

static lfs_t lfs;
static struct lfs_config cfg;
static uint8_t readbuf[FS_CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t progbuf[FS_CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t lookbuf[FS_CACHE_SIZE] __attribute__((aligned(8)));

/* Bounce buffer for sector-unaligned littlefs reads/writes.  littlefs may
   request a whole block (4096 bytes) at once, so this must hold one block
   (= 8 sectors). */
static uint8_t bounce[FS_BLOCK_SIZE] __attribute__((aligned(4)));

/* readdir is index based and every user of it walks the whole directory in
   order, so the entries are served from a small snapshot instead of being
   re-derived per call.  Walking the tree for every index made listing the
   14 boot files read the directory 14 times: 1697 block reads and 1.8 MB
   through memcpy to print 14 names, which is 0.25 s of boot on qemu-virt
   (8 s when QEMU's TCG has one of its slow days).  Any mutation drops the
   snapshot, so a listing can never go stale. */
#define DENT_MAX  20
#define DENT_NAME 32
/* Names only: sizes come from stat(), which asks littlefs directly rather
   than a snapshot taken at an unknown time. */
static char     dents[DENT_MAX][DENT_NAME];
static int      dent_count = -1;   /* -1 while there is no snapshot */
static int      dent_fill;         /* entries seen by the last walk  */
static int      dent_active;       /* the walk is filling the table  */
static int      dent_overflow;     /* does not fit: walk every time  */

static int      enum_target;
static int      enum_index;
static int      enum_hit;
static char     enum_name[96];
static int      ok;

/* One lock for the whole filesystem, held for the duration of an API call.
 *
 * The shared state is bigger than "the disk": littlefs keeps one set of block
 * caches in `cfg` (read/prog/lookahead), this file adds a 4 KiB bounce buffer
 * for unaligned sector I/O, and both the handle table and the readdir
 * snapshot are process-independent globals.  Two harts in lfs_file_read on
 * different files would scramble each other's cache blocks, so the unit of
 * exclusion cannot be the block - it has to be the call.  This is what
 * LFS_THREADSAFE would give us, plus the parts littlefs knows nothing about
 * (handles, snapshot), which is why it lives here and not in `cfg`.
 *
 * On a uniprocessor this lock is a no-op, and that is correct rather than
 * lucky: multitasking exists there, but a task only ever loses the CPU in
 * user mode or at a blocking call (process_tick refuses to preempt a kernel
 * frame), so no second task can observe half an lfs call.  The same two
 * preconditions make the no-op safe, and both would have to be revisited
 * before either changes:
 *   - a tick that lands in kernel mode only accounts, never switches;
 *   - nothing called here blocks (a future blocking device read has to
 *     release the lock before it sleeps).
 *
 * Not held across anything that blocks: every call below is synchronous
 * memory or block-device work, so the holder always runs to the unlock and
 * interrupts stay on.
 *
 * Ordering: fs_lock -> heap_lock, because littlefs allocates through
 * kmalloc.  Nothing takes them the other way round. */
static spin_t fs_lock = SPIN_INIT;

static ofile_t openfiles[MAX_OPEN];

static uint32_t block_lba(lfs_block_t block, lfs_off_t off)
{
    uint32_t byte = (uint32_t)block * FS_BLOCK_SIZE + (uint32_t)off;
    return PLAT_LFS_LBA + (byte >> 9);
}

/* ---- block device ------------------------------------------------------
 *
 * Counters behind /proc/lfs.  Cheap enough not to guard behind a #ifdef:
 * five increments per block-device call, next to a 4 KiB flash operation.
 *
 * There used to be a 512-byte write-coalescing buffer here, to absorb the
 * four-byte CTZ pointer updates littlefs makes while a file grows.  Measured
 * on a C3 it never carried a single byte: littlefs's prog_size equals the
 * device sector, so every prog arrives 512-aligned and goes straight to the
 * device.  The counters are what showed that, so they stayed and the buffer
 * did not. */
static struct lfs_io_stats io_stats;

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)c;
    uint32_t byte = (uint32_t)block * FS_BLOCK_SIZE + (uint32_t)off;
    uint32_t first = byte >> 9;
    uint32_t last = (byte + size - 1) >> 9;
    uint32_t count = last - first + 1;
    if (count > FS_BLOCK_SIZE / 512) {
        kprintf("[lfs] read FAIL count=%u block=%u off=%u size=%u\n",
                count, (uint32_t)block, (uint32_t)off, (uint32_t)size);
        return LFS_ERR_IO;
    }

    io_stats.reads++;
    io_stats.read_sectors += count;
    /* The kernel runs with interrupts off, so a flash read is a window in
     * which nobody drains the console's 128-byte receive FIFO; at 115200
     * that FIFO holds 11 ms of input and one sector read is about half of
     * it.  Exec reads a whole program this way, and the paste that is
     * arriving meanwhile lost its tail (a 24-line paste ran 4 commands and
     * the receiver then overran).  tty_poll() touches only the console
     * queues - no lock, no filesystem - so draining per sector brings the
     * window down to something the FIFO absorbs. */
    tty_poll();
    if (plat_block_read(PLAT_LFS_LBA + first, count, bounce) != 0) {
        kprintf("[lfs] read FAIL block=%u off=%u size=%u abs_lba=%u count=%u\n",
                (uint32_t)block, (uint32_t)off, (uint32_t)size,
                PLAT_LFS_LBA + first, count);
        return LFS_ERR_IO;
    }

    memcpy(buffer, bounce + (byte & 0x1FF), size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)c;
    uint32_t byte = (uint32_t)block * FS_BLOCK_SIZE + (uint32_t)off;
    uint32_t first = byte >> 9;
    uint32_t last = (byte + size - 1) >> 9;
    uint32_t count = last - first + 1;
    if (count > FS_BLOCK_SIZE / 512)
        return LFS_ERR_IO;

    io_stats.progs++;
    io_stats.prog_bytes += size;
    tty_poll();                       /* see bd_read: same window, same fix */

    /* A littlefs prog is sector aligned in the common case (prog_size and
     * the device sector are both 512), and then the read-modify-write below
     * is pure overhead: it reads a sector that is about to be overwritten.
     * Going straight through halved the sector reads on a 3 MiB write. */
    if ((byte & 0x1FF) == 0 && (size & 0x1FF) == 0)
        return plat_block_write(PLAT_LFS_LBA + first, size >> 9, buffer) == 0
                   ? 0 : LFS_ERR_IO;

    /* Unaligned: read the sectors in, patch, write them back. */
    if (plat_block_read(PLAT_LFS_LBA + first, count, bounce) != 0)
        return LFS_ERR_IO;
    memcpy(bounce + (byte & 0x1FF), buffer, size);
    if (plat_block_write(PLAT_LFS_LBA + first, count, bounce) != 0)
        return LFS_ERR_IO;
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;
    tty_poll();                       /* see bd_read: an erase is ~20 ms  */
    uint32_t lba = PLAT_LFS_LBA + ((uint32_t)block * FS_BLOCK_SIZE >> 9);
    uint32_t n = FS_BLOCK_SIZE / 512;

    io_stats.erases++;

    /* The platform turns the whole 4 KiB block back to the erased value:
       a real NOR erase on flash-backed boards (writing 0xFF over
       programmed bits would be a no-op there), an 0xFF fill on RAM or
       plain-write media. */
    if (plat_block_erase(lba, n) != 0)
        return LFS_ERR_IO;
    return 0;
}

static int bd_sync(const struct lfs_config *c)
{
    (void)c;
    /* Every prog already reached the device, so there is nothing to flush.
     * (A write-back cache here was tried and measured: littlefs's
     * prog_size equals the sector size, so it never carried a byte.) */
    return 0;
}

/* Exercise the write path once at boot so a broken ATA write shows up
   immediately instead of silently corrupting the filesystem later. */
#ifndef TOS_LFS_RO
static void write_selftest(void)
{
    const char *path = "selftest.tmp";
    const char *msg = "block device write OK\n";
    uint8_t out[FS_CACHE_SIZE];          /* littlefs cache buffer: >= cache_size */
    uint8_t tmp[64];
    lfs_file_t f;
    struct lfs_file_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.buffer = out;

    if (lfs_file_opencfg(&lfs, &f, path, LFS_O_CREAT | LFS_O_WRONLY, &fc) < 0) {
        kprintf("[ramdisk] write test: create failed\n");
        return;
    }
    lfs_file_write(&lfs, &f, msg, (lfs_size_t)(strlen(msg) + 1));
    lfs_file_close(&lfs, &f);

    lfs_ssize_t n = -1;
    if (lfs_file_opencfg(&lfs, &f, path, LFS_O_RDONLY, &fc) < 0) {
        kprintf("[ramdisk] write test: reopen failed\n");
    } else {
        memset(tmp, 0, sizeof(tmp));
        n = lfs_file_read(&lfs, &f, tmp, sizeof(tmp));
        lfs_file_close(&lfs, &f);
    }
    lfs_remove(&lfs, path);

    if (n > 0 && strcmp((char *)tmp, msg) == 0)
        kprintf("[ramdisk] block device write test passed\n");
    else
        kprintf("[ramdisk] block device write test FAILED\n");
}
#endif  /* !TOS_LFS_RO */

/* Walk one directory level only: list the immediate children of `dirpath`
   as entries and do NOT descend into subdirectories.  This keeps `ls`
   scalable as the tree grows instead of dumping the whole hierarchy.
   Returns 1 as soon as the entry at enum_target has been recorded; with
   enum_target < 0 it walks the level, leaving the total in enum_index. */
static int enum_dir(const char *dirpath)
{
    lfs_dir_t dir;
    if (lfs_dir_open(&lfs, &dir, dirpath) < 0)
        return 0;

    struct lfs_info info;
    while (lfs_dir_read(&lfs, &dir, &info) > 0) {
        if (info.name[0] == '.')
            continue;

        /* Record the bare entry name; the caller (ls) turns it into a
           path against the directory it asked to list.  Storing the bare
           name keeps root and subdirectory listings identical in shape. */
        if (info.type == LFS_TYPE_DIR) {
            /* List the directory itself, but do not descend. */
            if (dent_active) {
                if (dent_fill < DENT_MAX) {
                    strncpy(dents[dent_fill], info.name, DENT_NAME - 1);
                    dents[dent_fill][DENT_NAME - 1] = '\0';
                }
                dent_fill++;
                enum_index++;
                continue;
            }
            if (enum_index == enum_target) {
                strncpy(enum_name, info.name, sizeof(enum_name) - 1);
                enum_name[sizeof(enum_name) - 1] = '\0';
                enum_hit = 1;
                lfs_dir_close(&lfs, &dir);
                return 1;
            }
            enum_index++;
            continue;
        }
        if (info.type != LFS_TYPE_REG)
            continue;

        if (dent_active) {
            if (dent_fill < DENT_MAX) {
                strncpy(dents[dent_fill], info.name, DENT_NAME - 1);
                dents[dent_fill][DENT_NAME - 1] = '\0';
            }
            dent_fill++;
            enum_index++;
            continue;
        }

        if (enum_index == enum_target) {
            strncpy(enum_name, info.name, sizeof(enum_name) - 1);
            enum_name[sizeof(enum_name) - 1] = '\0';
            enum_hit = 1;
            lfs_dir_close(&lfs, &dir);
            return 1;
        }
        enum_index++;
    }
    lfs_dir_close(&lfs, &dir);
    return 0;
}

static void enum_start(void)
{
    enum_index = 0;
    enum_hit = 0;
    enum_name[0] = '\0';
}

/* One pass over the tree fills the table.  Returns the entry count, or -1
   when there are more entries than DENT_MAX (the caller then falls back to
   walking per call, which stays correct). */
static int dent_build(void)
{
    if (dent_count >= 0)
        return dent_count;
    if (dent_overflow)
        return -1;

    dent_active = 1;
    dent_fill = 0;
    enum_start();
    enum_target = -1;
    enum_dir("/");
    dent_active = 0;

    if (dent_fill > DENT_MAX) {
        dent_overflow = 1;
        return -1;
    }
    dent_count = dent_fill;
    return dent_count;
}

/* A create, truncate or write changes what a listing would say. */
static void dent_drop(void)
{
    dent_count = -1;
    dent_overflow = 0;
}

void ramdisk_init(void)
{
    heap_init();


    memset(&cfg, 0, sizeof(cfg));
    cfg.read  = bd_read;
    cfg.prog  = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync  = bd_sync;

    cfg.read_size      = 512;             /* one disk sector */
    cfg.prog_size      = 512;
    cfg.block_size     = FS_BLOCK_SIZE;
    /* block_count=0 tells littlefs to take the size from the image's
     * superblock instead of checking it against the device (lfs.c: "this
     * is where we get the block_count from disk if block_count=0").  The
     * shipped image is built small (RamdiskSectors in build.go) and
     * lfs_fs_grow below re-commits the superblock for whatever the real
     * partition says, so a 300 KiB flash image grows into the rest of a
     * 4 MiB chip on first boot.  When image and device already agree the
     * grow is a no-op, so every board runs the same code. */
    cfg.block_count    = 0;
    cfg.cache_size     = FS_CACHE_SIZE;
    cfg.lookahead_size = FS_CACHE_SIZE;
    cfg.block_cycles   = 100;

    cfg.read_buffer      = readbuf;
    cfg.prog_buffer      = progbuf;
    cfg.lookahead_buffer = lookbuf;

    ok = 0;
    dent_drop();
    enum_start();
    memset(openfiles, 0, sizeof(openfiles));

    if (plat_block_count() == 0 && plat_block_read(PLAT_LFS_LBA, 1, bounce) != 0) {
        kprintf("[ramdisk] no block device, filesystem unavailable\n");
        return;
    }

    if (lfs_mount(&lfs, &cfg) < 0) {
        kprintf("[ramdisk] littlefs mount failed (partition at LBA %u?)\n",
                PLAT_LFS_LBA);
        return;
    }

#ifndef TOS_LFS_RO
    /* Grow to the real partition size.  littlefs 2.9 decides free blocks by
     * tree reachability (lfs_alloc_scan walks every referenced block), not
     * by erased state, and erases a block the first time it allocates it -
     * so a tail that still holds an old image is simply reclaimed block by
     * block, no erase pass needed.  Failure keeps the (smaller) mounted
     * filesystem working; only the extra space is lost. */
    if (lfs_fs_grow(&lfs, PLAT_LFS_BLOCKS) < 0)
        kprintf("[ramdisk] warning: fs grow to %u blocks failed\n",
                PLAT_LFS_BLOCKS);
#endif

    /* Readdir walks the tree on demand, so there is nothing to snapshot. */
    ok = 1;

#ifndef TOS_LFS_RO
    write_selftest();
#endif
}

int ramdisk_ok(void)
{
    return ok;
}

static uint32_t count_locked(void)
{
    if (!ok)
        return 0;
    int n = dent_build();
    if (n >= 0)
        return (uint32_t)n;
    enum_start();
    enum_target = -1;              /* never matches: walk everything */
    enum_dir("/");
    return (uint32_t)enum_index;
}

/* The index-th name in the root snapshot (or a full walk when the snapshot
 * does not fit).  Sizes are not part of the name API any more: `ls` gets them
 * from stat(), which asks littlefs directly instead of trusting a snapshot
 * taken at an unknown time. */
static const char *name_locked(uint32_t index)
{
    if (!ok)
        return NULL;
    if (dent_build() >= 0) {
        if ((int)index >= dent_count)
            return NULL;
        return dents[index];
    }
    enum_start();
    enum_target = (int)index;
    enum_dir("/");
    if (!enum_hit)
        return NULL;
    return enum_name;
}

/* List a given directory (not just the root).  The root reuses the snapshot
 * built by dent_build(); every other directory is walked directly, which is
 * correct and cheap for the small directories TinyOS has.  Names are bare
 * entries (see enum_dir), so the caller builds stat paths itself. */
static int list_count_locked(const char *dirpath)
{
    if (!ok)
        return 0;
    if (strcmp(dirpath, "/") == 0)
        return (int)count_locked();
    enum_start();
    enum_target = -1;              /* never matches: walk everything */
    enum_dir(dirpath);
    return (int)enum_index;
}

static const char *list_name_locked(const char *dirpath, uint32_t index)
{
    if (!ok)
        return NULL;
    if (strcmp(dirpath, "/") == 0)
        return name_locked(index);
    enum_start();
    enum_target = (int)index;
    enum_dir(dirpath);
    if (!enum_hit)
        return NULL;
    return enum_name;
}

/* Stat a path directly (no traversal of the listing snapshot).  Returns 0
   and fills *size/*type on success, -1 on error (e.g. ENOENT).  Used by the
   SYS_STAT syscall so `ls` can show per-entry size and type without the
   kernel walking the tree. */
static int stat_locked(const char *path, uint32_t *size, uint8_t *type)
{
    if (!ok)
        return -1;
    struct lfs_info info;
    if (lfs_stat(&lfs, path, &info) < 0)
        return -1;
    if (size)
        *size = info.size;
    if (type)
        *type = (info.type == LFS_TYPE_DIR) ? 1 : 0;
    return 0;
}

/* Remove a file.  Two refusals, both for good reason:
 *
 *   - directories: lfs_remove() deletes a directory *recursively*, so a
 *     stray `rm /bin` would take the shell with it.  Nothing in the system
 *     creates a directory at runtime anyway.
 *   - open files: littlefs would update the parent's metadata and leave the
 *     live handle pointing at blocks that are no longer in the tree, so the
 *     next write goes to free space and the file "comes back" as garbage.
 *     Compared by path, which is why ofile_t carries one.
 *
 * The snapshot is dropped: a listing taken before the removal must not
 * survive it. */
#ifndef TOS_LFS_RO
static int unlink_locked(const char *path)
{
    if (!ok)
        return -1;

    struct lfs_info info;
    if (lfs_stat(&lfs, path, &info) < 0)
        return -1;
    if (info.type == LFS_TYPE_DIR)
        return -1;

    for (int i = 0; i < MAX_OPEN; i++) {
        if (openfiles[i].used && !strcmp(openfiles[i].path, path))
            return -1;
    }

    dent_drop();
    return lfs_remove(&lfs, path) < 0 ? -1 : 0;
}
#endif

/* Report littlefs partition usage.  `total` is the configured volume size;
   `used` is what littlefs has actually consumed.  Note lfs_fs_size()
   returns a *block count*, so multiply by the block size for bytes.
   Either pointer may be NULL.  Returns 0 when the filesystem is mounted. */
static int df_locked(uint32_t *total, uint32_t *used)
{
    if (total)
        *total = (uint32_t)FS_BLOCK_SIZE * PLAT_LFS_BLOCKS;
    if (used) {
        if (!ok) {
            *used = 0;
        } else {
            lfs_ssize_t n = lfs_fs_size(&lfs);
            *used = (n < 0) ? 0 : (uint32_t)n * FS_BLOCK_SIZE;
        }
    }
    return ok ? 0 : -1;
}

/* Shared body of every open.  The flags decide read-only, read-write or
 * truncating; everything else (a cache buffer, a free handle, the size) is
 * the same and used to be copied three times. */
static int open_with(const char *path, int flags)
{
    if (!ok)
        return -1;

    int h = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!openfiles[i].used) {
            h = i;
            break;
        }
    }
    if (h < 0)
        return -1;

    memset(&openfiles[h].fc, 0, sizeof(struct lfs_file_config));
    openfiles[h].cache = kmalloc(FS_CACHE_SIZE);
    if (!openfiles[h].cache) {
        kprintf("open %s: out of memory for file cache\n", path);
        return -1;
    }
    openfiles[h].fc.buffer = openfiles[h].cache;
    if (lfs_file_opencfg(&lfs, &openfiles[h].f, path, flags,
                         &openfiles[h].fc) < 0) {
        kfree(openfiles[h].cache);
        openfiles[h].cache = NULL;
        return -1;
    }

    openfiles[h].used = 1;
    openfiles[h].size = (uint32_t)lfs_file_size(&lfs, &openfiles[h].f);
    strncpy(openfiles[h].path, path, sizeof(openfiles[h].path) - 1);
    openfiles[h].path[sizeof(openfiles[h].path) - 1] = '\0';
    return h;
}

int ramdisk_open(const char *path)
{
    int h;

    spin_lock(&fs_lock);
    h = open_with(path, LFS_O_RDONLY);
    spin_unlock(&fs_lock);
    return h;
}

/* Read-write, without truncating.  This is what a child gets when it inherits
 * a descriptor the parent opened for writing (`echo hi > f`): the child must
 * be able to append to what the shell set up, and a plain ramdisk_open()
 * would hand it a read-only handle that fails on the first write. */
int ramdisk_open_rw(const char *path)
{
#ifdef TOS_LFS_RO
    (void)path;
    return -1;
#else
    int h;

    spin_lock(&fs_lock);
    h = open_with(path, LFS_O_RDWR);
    spin_unlock(&fs_lock);
    return h;
#endif
}

/* Create (or truncate) a file for writing.  Opened read-write so the
   caller can verify what it wrote. */
#ifndef TOS_LFS_RO
static int create_locked(const char *path)
{
    if (!ok)
        return -1;

    int h = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!openfiles[i].used) {
            h = i;
            break;
        }
    }
    if (h < 0)
        return -1;

    /* The directory may gain an entry, or lose one via truncation of an
       existing name: either way the snapshot is stale. */
    dent_drop();

    memset(&openfiles[h].fc, 0, sizeof(struct lfs_file_config));
    openfiles[h].cache = kmalloc(FS_CACHE_SIZE);
    if (!openfiles[h].cache) {
        kprintf("create %s: out of memory for file cache\n", path);
        return -1;
    }
    openfiles[h].fc.buffer = openfiles[h].cache;
    if (lfs_file_opencfg(&lfs, &openfiles[h].f, path,
                         LFS_O_CREAT | LFS_O_TRUNC | LFS_O_RDWR,
                         &openfiles[h].fc) < 0) {
        kfree(openfiles[h].cache);
        openfiles[h].cache = NULL;
        return -1;
    }

    openfiles[h].used = 1;
    openfiles[h].size = (uint32_t)lfs_file_size(&lfs, &openfiles[h].f);
    strncpy(openfiles[h].path, path, sizeof(openfiles[h].path) - 1);
    openfiles[h].path[sizeof(openfiles[h].path) - 1] = '\0';
    return h;
}
#endif  /* !TOS_LFS_RO */

static uint32_t size_locked(int h)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return 0;
    return openfiles[h].size;
}

static int read_locked(int h, void *buf, uint32_t len)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return -1;
    lfs_ssize_t n = lfs_file_read(&lfs, &openfiles[h].f, buf, len);
    return n < 0 ? -1 : (int)n;
}

#ifndef TOS_LFS_RO
static int write_locked(int h, const void *buf, uint32_t len)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return -1;
    lfs_ssize_t n = lfs_file_write(&lfs, &openfiles[h].f, buf, len);
    if (n > 0) {
        openfiles[h].size += (uint32_t)n;
        dent_drop();          /* the listed size just changed */
    }
    return n < 0 ? -1 : (int)n;
}
#endif

/* Move the handle's position.  A duplicated descriptor keeps the position it
 * was copied from (fd_clone), but the new littlefs handle starts at 0, so
 * "same position" has to be re-established rather than assumed. */
static int seek_locked(int h, uint32_t pos)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return -1;
    if (lfs_file_seek(&lfs, &openfiles[h].f, (lfs_soff_t)pos,
                      LFS_SEEK_SET) < 0)
        return -1;
    return 0;
}

static void close_locked(int h)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return;
    lfs_file_close(&lfs, &openfiles[h].f);
    kfree(openfiles[h].cache);
    memset(&openfiles[h], 0, sizeof(ofile_t));
}

/* ---- the lock boundary ------------------------------------------------
 *
 * Everything above is `_locked`: the body as it was, with no locking of its
 * own, so one entry point can call another (list_name -> name, list_count ->
 * count) without re-entering a lock that is not recursive.  Callers outside
 * this file only ever see the wrappers below.
 *
 * The alternative - a lock/unlock pair inside each body - reads as less code
 * until the first `return` in the middle of an lfs call is added and one path
 * forgets to unlock.  One wrapper per entry point is a shape the compiler can
 * see through: miss one and the symbol is simply missing at link time. */

/* Copy the index-th name into the caller's buffer while the lock is still
 * held.  Returning the pointer instead (what this used to do) reads as
 * cheaper but hands out a shared static: the second CPU enumerating anything
 * would rewrite it before the caller's memcpy. */
int ramdisk_list_name(const char *dirpath, uint32_t index, char *buf, int cap)
{
    const char *name;
    int n;

    spin_lock(&fs_lock);
    name = list_name_locked(dirpath, index);
    n = name ? (int)strlen(name) : 0;
    if (name && n >= cap)
        n = -1;                 /* does not fit: caller sees EOF, not a stub */
    else if (name)
        memcpy(buf, name, (size_t)n + 1);
    spin_unlock(&fs_lock);
    return n;
}

int ramdisk_list_count(const char *dirpath)
{
    int n;

    spin_lock(&fs_lock);
    n = list_count_locked(dirpath);
    spin_unlock(&fs_lock);
    return n;
}

int ramdisk_stat(const char *path, uint32_t *size, uint8_t *type)
{
    int r;

    spin_lock(&fs_lock);
    r = stat_locked(path, size, type);
    spin_unlock(&fs_lock);
    return r;
}

int ramdisk_unlink(const char *path)
{
#ifdef TOS_LFS_RO
    (void)path;
    return -1;
#else
    int r;

    spin_lock(&fs_lock);
    r = unlink_locked(path);
    spin_unlock(&fs_lock);
    return r;
#endif
}

void ramdisk_io_stats(struct lfs_io_stats *s)
{
    spin_lock(&fs_lock);
    *s = io_stats;
    spin_unlock(&fs_lock);
}

int ramdisk_df(uint32_t *total, uint32_t *used)
{
    int r;

    spin_lock(&fs_lock);
    r = df_locked(total, used);
    spin_unlock(&fs_lock);
    return r;
}

int ramdisk_create(const char *path)
{
#ifdef TOS_LFS_RO
    (void)path;
    return -1;
#else
    int h;

    spin_lock(&fs_lock);
    h = create_locked(path);
    spin_unlock(&fs_lock);
    return h;
#endif
}

uint32_t ramdisk_size(int h)
{
    uint32_t n;

    spin_lock(&fs_lock);
    n = size_locked(h);
    spin_unlock(&fs_lock);
    return n;
}

int ramdisk_read(int h, void *buf, uint32_t len)
{
    int n;

    spin_lock(&fs_lock);
    n = read_locked(h, buf, len);
    spin_unlock(&fs_lock);
    return n;
}

int ramdisk_write(int h, const void *buf, uint32_t len)
{
#ifdef TOS_LFS_RO
    (void)h; (void)buf; (void)len;
    return -1;
#else
    int n;

    spin_lock(&fs_lock);
    n = write_locked(h, buf, len);
    spin_unlock(&fs_lock);
    return n;
#endif
}

int ramdisk_seek(int h, uint32_t pos)
{
    int r;

    spin_lock(&fs_lock);
    r = seek_locked(h, pos);
    spin_unlock(&fs_lock);
    return r;
}

void ramdisk_close(int h)
{
    spin_lock(&fs_lock);
    close_locked(h);
    spin_unlock(&fs_lock);
}
