#include "ramdisk.h"
#include "string.h"
#include "console.h"
#include "heap.h"
#include "platform.h"
#include "lfs_config.h"   /* must come before lfs.h: defines LFS_NO_* + LFS_MALLOC */
#include "lfs.h"

/* The littlefs partition lives on the ATA disk at a fixed LBA offset (it is
   written there by the build tool, right after the kernel image).  The
   kernel reads it on demand through the ATA PIO driver - no ramdisk blob is
   copied into memory at boot.  Writes (prog/erase) go back to the disk, so
   the filesystem is genuinely writable. */

#define FS_BLOCK_SIZE  4096
#define FS_BLOCK_COUNT 75              /* 75 * 4 KiB == 300 KiB == 600 sectors */
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
} ofile_t;

static lfs_t lfs;
static struct lfs_config cfg;
static uint8_t readbuf[FS_CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t progbuf[FS_CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t lookbuf[FS_CACHE_SIZE] __attribute__((aligned(8)));

/* Bounce buffer for sector-unaligned littlefs reads/writes.  littlefs may
   request a whole block (4096 bytes) at once, so this must hold one block
   (= 8 sectors). */
static uint8_t bounce[FS_BLOCK_SIZE];

/* readdir is index based, but no snapshot table is kept: each call walks
   the (tiny) directory tree and stops at the requested entry.  Costs a few
   directory reads per call and saves a 4 KiB array in .bss. */
static int      enum_target;
static int      enum_index;
static int      enum_hit;
static uint32_t enum_size;
static char     enum_name[96];
static int      ok;

static ofile_t openfiles[MAX_OPEN];

static uint32_t block_lba(lfs_block_t block, lfs_off_t off)
{
    uint32_t byte = (uint32_t)block * FS_BLOCK_SIZE + (uint32_t)off;
    return LFS_PARTITION_LBA + (byte >> 9);
}

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

    if (plat_block_read(LFS_PARTITION_LBA + first, count, bounce) != 0) {
        kprintf("[lfs] read FAIL block=%u off=%u size=%u abs_lba=%u count=%u\n",
                (uint32_t)block, (uint32_t)off, (uint32_t)size,
                LFS_PARTITION_LBA + first, count);
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

    /* Read-modify-write: a littlefs prog is rarely sector aligned. */
    if (plat_block_read(LFS_PARTITION_LBA + first, count, bounce) != 0)
        return LFS_ERR_IO;
    memcpy(bounce + (byte & 0x1FF), buffer, size);
    if (plat_block_write(LFS_PARTITION_LBA + first, count, bounce) != 0)
        return LFS_ERR_IO;
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;
    /* littlefs treats 0xFF as the erased value (NOR flash semantics); fill
       the whole block so stale data can never be mistaken for new.
       Reuse the shared bounce buffer instead of a second 4 KiB array. */
    memset(bounce, 0xFF, sizeof(bounce));
    uint32_t lba = LFS_PARTITION_LBA + ((uint32_t)block * FS_BLOCK_SIZE >> 9);
    if (plat_block_write(lba, FS_BLOCK_SIZE / 512, bounce) != 0)
        return LFS_ERR_IO;
    return 0;
}

static int bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

/* Exercise the write path once at boot so a broken ATA write shows up
   immediately instead of silently corrupting the filesystem later. */
static void write_selftest(void)
{
    const char *path = "selftest.tmp";
    const char *msg = "ATA PIO write OK\n";
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
        kprintf("[ramdisk] ATA write test passed\n");
    else
        kprintf("[ramdisk] ATA write test FAILED\n");
}

/* Recursively walk the directory tree so "bin/hello" is visible even though
   the shell and the exec path use full paths.  Depth is shallow.
   Returns 1 as soon as the entry at enum_target has been recorded; with
   enum_target < 0 it walks everything, leaving the total in enum_index. */
static int enum_dir(const char *dirpath)
{
    lfs_dir_t dir;
    if (lfs_dir_open(&lfs, &dir, dirpath) < 0)
        return 0;

    struct lfs_info info;
    while (lfs_dir_read(&lfs, &dir, &info) > 0) {
        if (info.name[0] == '.')
            continue;

        char full[96];
        if (strcmp(dirpath, "/") == 0) {
            strncpy(full, info.name, sizeof(full) - 1);
            full[sizeof(full) - 1] = '\0';
        } else {
            size_t dl = strlen(dirpath);
            if (dl > sizeof(full) - 2)
                dl = sizeof(full) - 2;
            memcpy(full, dirpath, dl);
            full[dl] = '/';
            strncpy(full + dl + 1, info.name, sizeof(full) - dl - 2);
            full[sizeof(full) - 1] = '\0';
        }

        if (info.type == LFS_TYPE_DIR) {
            if (enum_dir(full)) {
                lfs_dir_close(&lfs, &dir);
                return 1;
            }
            continue;
        }
        if (info.type != LFS_TYPE_REG)
            continue;

        if (enum_index == enum_target) {
            strncpy(enum_name, full, sizeof(enum_name) - 1);
            enum_name[sizeof(enum_name) - 1] = '\0';
            enum_size = info.size;
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
    enum_size = 0;
    enum_name[0] = '\0';
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
    cfg.block_count    = FS_BLOCK_COUNT;
    cfg.cache_size     = FS_CACHE_SIZE;
    cfg.lookahead_size = FS_CACHE_SIZE;
    cfg.block_cycles   = 100;

    cfg.read_buffer      = readbuf;
    cfg.prog_buffer      = progbuf;
    cfg.lookahead_buffer = lookbuf;

    ok = 0;
    enum_start();
    memset(openfiles, 0, sizeof(openfiles));

    if (plat_block_count() == 0 && plat_block_read(LFS_PARTITION_LBA, 1, bounce) != 0) {
        kprintf("[ramdisk] no block device, filesystem unavailable\n");
        return;
    }

    if (lfs_mount(&lfs, &cfg) < 0) {
        kprintf("[ramdisk] littlefs mount failed (partition at LBA %u?)\n",
                LFS_PARTITION_LBA);
        return;
    }

    /* Readdir walks the tree on demand, so there is nothing to snapshot. */
    ok = 1;

    write_selftest();
}

int ramdisk_ok(void)
{
    return ok;
}

uint32_t ramdisk_count(void)
{
    if (!ok)
        return 0;
    enum_start();
    enum_target = -1;              /* never matches: walk everything */
    enum_dir("/");
    return (uint32_t)enum_index;
}

const char *ramdisk_name(uint32_t index, uint32_t *size)
{
    if (!ok)
        return NULL;
    enum_start();
    enum_target = (int)index;
    enum_dir("/");
    if (!enum_hit)
        return NULL;
    if (size)
        *size = enum_size;
    return enum_name;
}

/* Load a whole file into a freshly kmalloc'd buffer.  Caller must call
   ramdisk_release() when done. */
int ramdisk_find(const char *name, uint8_t **data, uint32_t *size)
{
    if (!ok)
        return 0;

    lfs_file_t f;
    struct lfs_file_config fc;
    uint8_t filebuf[FS_CACHE_SIZE];

    memset(&fc, 0, sizeof(fc));
    fc.buffer = filebuf;
    if (lfs_file_opencfg(&lfs, &f, name, LFS_O_RDONLY, &fc) < 0)
        return 0; /* normal miss (ENOENT): caller reports it */

    uint32_t sz = (uint32_t)lfs_file_size(&lfs, &f);
    uint8_t *buf = kmalloc(sz ? sz : 1);
    if (!buf) {
        kprintf("find %s: kmalloc(%u) failed\n", name, sz);
        lfs_file_close(&lfs, &f);
        return 0;
    }

    uint32_t got = 0;
    while (got < sz) {
        lfs_ssize_t n = lfs_file_read(&lfs, &f, buf + got, sz - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    lfs_file_close(&lfs, &f);

    if (got != sz) {
        kprintf("find %s: short read %u/%u\n", name, got, sz);
        kfree(buf);
        return 0;
    }
    *data = buf;
    *size = sz;
    return 1;
}

void ramdisk_release(void *data)
{
    kfree(data);
}

int ramdisk_open(const char *path)
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
    if (lfs_file_opencfg(&lfs, &openfiles[h].f, path, LFS_O_RDONLY,
                         &openfiles[h].fc) < 0) {
        kfree(openfiles[h].cache);
        openfiles[h].cache = NULL;
        return -1;
    }

    openfiles[h].used = 1;
    openfiles[h].size = (uint32_t)lfs_file_size(&lfs, &openfiles[h].f);
    return h;
}

/* Create (or truncate) a file for writing.  Opened read-write so the
   caller can verify what it wrote. */
int ramdisk_create(const char *path)
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
    return h;
}

uint32_t ramdisk_size(int h)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return 0;
    return openfiles[h].size;
}

int ramdisk_read(int h, void *buf, uint32_t len)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return -1;
    lfs_ssize_t n = lfs_file_read(&lfs, &openfiles[h].f, buf, len);
    return n < 0 ? -1 : (int)n;
}

int ramdisk_write(int h, const void *buf, uint32_t len)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return -1;
    lfs_ssize_t n = lfs_file_write(&lfs, &openfiles[h].f, buf, len);
    if (n > 0)
        openfiles[h].size += (uint32_t)n;
    return n < 0 ? -1 : (int)n;
}

void ramdisk_close(int h)
{
    if (h < 0 || h >= MAX_OPEN || !openfiles[h].used)
        return;
    lfs_file_close(&lfs, &openfiles[h].f);
    kfree(openfiles[h].cache);
    memset(&openfiles[h], 0, sizeof(ofile_t));
}
