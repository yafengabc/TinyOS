/*
 * mklfs - host-side tool that builds (and verifies) the TinyOS littlefs
 *         image with the very same littlefs the kernel mounts.
 *
 * This is a normal host program (compiled with `zig cc` for the native
 * target), not kernel code.  It exists so the on-disk format is produced
 * and validated by littlefs itself instead of a hand-rolled formatter.
 *
 * usage:
 *   mklfs <image> <size-bytes> <base-dir> <name> [<name> ...]
 *       create a littlefs image from base-dir/<name> for each name.
 *   mklfs -v <image>
 *       mount <image>, list the root directory, read every regular file
 *       and report name + size.  Fails (non-zero) on any error.
 *
 * The partition parameters must match kernel/ramdisk.c:
 *   block_size  = 4096
 *   block_count = size / 4096
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "lfs.h"

#define BLOCK_SIZE 4096
#define CACHE_SIZE 512

static uint8_t *disk;
static lfs_t    lfs;
static struct lfs_config cfg;
static uint8_t readbuf[CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t progbuf[CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t lookbuf[CACHE_SIZE] __attribute__((aligned(8)));

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)c;
    memcpy(buffer, disk + (size_t)block * BLOCK_SIZE + off, size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)c;
    memcpy(disk + (size_t)block * BLOCK_SIZE + off, buffer, size);
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;
    memset(disk + (size_t)block * BLOCK_SIZE, 0xFF, BLOCK_SIZE);
    return 0;
}

static int bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

static void cfg_init(lfs_size_t block_count)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.read  = bd_read;
    cfg.prog  = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync  = bd_sync;

    cfg.read_size      = 16;
    cfg.prog_size      = 16;
    cfg.block_size     = BLOCK_SIZE;
    cfg.block_count    = block_count;
    cfg.cache_size     = CACHE_SIZE;
    cfg.lookahead_size = CACHE_SIZE;
    cfg.block_cycles   = 100;

    cfg.read_buffer      = readbuf;
    cfg.prog_buffer      = progbuf;
    cfg.lookahead_buffer = lookbuf;
}

static int mkdirs(const char *path)
{
    /* Create every parent directory of a path like "bin/cat". */
    char tmp[128];
    if (strlen(path) >= sizeof(tmp))
        return -1;
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        int err = lfs_mkdir(&lfs, tmp);
        if (err < 0 && err != LFS_ERR_EXIST)
            return -1;   /* existing directories are fine */
        *p = '/';
    }
    return 0;
}

static int has_slash(const char *path)
{
    return strchr(path, '/') != NULL;
}
int lz4_compress_bound(int n);
int lz4_compress_block(const uint8_t *src, int src_len, uint8_t *dst);

#define LZ4_WRAP_HEADER 16

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Only ELF programs are compressed; data files stay as they are. */
static int looks_like_elf(const uint8_t *d, size_t n)
{
    return n >= 4 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F';
}

static int write_file(const char *name, const char *src)
{
    FILE *f = fopen(src, "rb");
    if (!f) {
        fprintf(stderr, "mklfs: cannot open %s: %s\n", src, strerror(errno));
        return -1;
    }

    /* read the whole file: compression needs the full buffer and programs
       are small */
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) {
        fclose(f);
        return -1;
    }
    uint8_t *data = malloc((size_t)flen ? (size_t)flen : 1);
    if (!data) {
        fclose(f);
        fprintf(stderr, "mklfs: out of memory reading %s\n", src);
        return -1;
    }
    if (flen > 0 && fread(data, 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f);
        free(data);
        fprintf(stderr, "mklfs: cannot read %s\n", src);
        return -1;
    }
    fclose(f);

    if (mkdirs(name) < 0) {
        free(data);
        fprintf(stderr, "mklfs: cannot create dirs for %s\n", name);
        return -1;
    }

    lfs_file_t file;
    if (lfs_file_open(&lfs, &file, name, LFS_O_CREAT | LFS_O_WRONLY) < 0) {
        free(data);
        fprintf(stderr, "mklfs: cannot create %s\n", name);
        return -1;
    }

    int rc = 0;

    if (looks_like_elf(data, (size_t)flen)) {
        int bound = lz4_compress_bound((int)flen);
        uint8_t *comp = malloc((size_t)bound);
        if (!comp) {
            fprintf(stderr, "mklfs: out of memory compressing %s\n", name);
            rc = -1;
            goto out;
        }
        int clen = lz4_compress_block(data, (int)flen, comp);

        uint8_t hdr[LZ4_WRAP_HEADER];
        memcpy(hdr, "LZ4TOS", 6);
        hdr[6] = 0;
        hdr[7] = 0;
        put_u32(hdr + 8, (uint32_t)flen);
        put_u32(hdr + 12, (uint32_t)clen);

        if (lfs_file_write(&lfs, &file, hdr, LZ4_WRAP_HEADER) != LZ4_WRAP_HEADER ||
            lfs_file_write(&lfs, &file, comp, (lfs_size_t)clen) != (lfs_ssize_t)clen) {
            fprintf(stderr, "mklfs: short write on %s\n", name);
            rc = -1;
        }
        free(comp);
        if (rc == 0)
            printf("  %-20s %7ld -> %7d bytes (%.0f%%)\n", name, flen, clen,
                   100.0 * clen / (flen ? flen : 1));
    } else {
        if (lfs_file_write(&lfs, &file, data, (lfs_size_t)flen) != (lfs_ssize_t)flen) {
            fprintf(stderr, "mklfs: short write on %s\n", name);
            rc = -1;
        }
    }

out:
    lfs_file_close(&lfs, &file);
    free(data);
    return rc;
}

static int create(int argc, char **argv)
{
    /* argv: [1]=image [2]=size [3]=base-dir [4..]=names */
    if (argc < 5) {
        fprintf(stderr, "usage: mklfs <image> <size> <base-dir> <name> [name...]\n");
        return 1;
    }

    long size = atol(argv[2]);
    if (size <= 0 || size % BLOCK_SIZE != 0) {
        fprintf(stderr, "mklfs: size must be a positive multiple of %d\n", BLOCK_SIZE);
        return 1;
    }

    disk = calloc(1, (size_t)size);
    if (!disk) {
        fprintf(stderr, "mklfs: out of memory\n");
        return 1;
    }

    cfg_init((lfs_size_t)(size / BLOCK_SIZE));

    if (lfs_format(&lfs, &cfg) < 0) {
        fprintf(stderr, "mklfs: lfs_format failed\n");
        free(disk);
        return 1;
    }
    if (lfs_mount(&lfs, &cfg) < 0) {
        fprintf(stderr, "mklfs: lfs_mount failed\n");
        free(disk);
        return 1;
    }

    int rc = 0;
    for (int i = 4; i < argc; i++) {
        char src[512];
        snprintf(src, sizeof(src), "%s/%s", argv[3], argv[i]);
        if (write_file(argv[i], src) < 0)
            rc = 1;
    }

    if (lfs_unmount(&lfs) < 0) {
        fprintf(stderr, "mklfs: lfs_unmount failed\n");
        rc = 1;
    }

    if (rc == 0) {
        FILE *out = fopen(argv[1], "wb");
        if (!out || fwrite(disk, 1, (size_t)size, out) != (size_t)size) {
            fprintf(stderr, "mklfs: cannot write %s\n", argv[1]);
            rc = 1;
        }
        if (out)
            fclose(out);
    }

    free(disk);
    return rc;
}

/* Recursively list a directory.  prefix "" means the root, otherwise a
   path like "bin".  level is only used to build child paths. */
static int dump_dir(const char *prefix, int level, const char *unused)
{
    (void)unused;
    (void)level;

    char dirpath[128];
    if (prefix[0] == '\0')
        snprintf(dirpath, sizeof(dirpath), "/");
    else
        snprintf(dirpath, sizeof(dirpath), "/%s", prefix);

    lfs_dir_t dir;
    if (lfs_dir_open(&lfs, &dir, dirpath) < 0) {
        fprintf(stderr, "mklfs: cannot open %s\n", dirpath);
        return -1;
    }

    int rc = 0;
    struct lfs_info info;
    while (lfs_dir_read(&lfs, &dir, &info) > 0) {
        if (info.name[0] == '.')
            continue;   /* skip "." and ".." */

        char path[128];
        if (prefix[0] == '\0')
            snprintf(path, sizeof(path), "%s", info.name);
        else
            snprintf(path, sizeof(path), "%s/%s", prefix, info.name);

        if (info.type == LFS_TYPE_DIR) {
            printf("%-24s <dir>\n", path);
            if (dump_dir(path, level + 1, "") < 0)
                rc = -1;
            continue;
        }

        /* Regular file: open it and read everything so a corrupt chain
           shows up as a failure instead of silently succeeding. */
        lfs_file_t f;
        struct lfs_file_config fc;
        uint8_t fbuf[CACHE_SIZE];
        memset(&fc, 0, sizeof(fc));
        fc.buffer = fbuf;
        if (lfs_file_opencfg(&lfs, &f, path, LFS_O_RDONLY, &fc) < 0) {
            fprintf(stderr, "mklfs: cannot open %s\n", path);
            rc = -1;
            continue;
        }
        uint8_t tmp[1024];
        lfs_ssize_t n;
        while ((n = lfs_file_read(&lfs, &f, tmp, sizeof(tmp))) > 0)
            ;
        int ok = n == 0;
        lfs_file_close(&lfs, &f);
        if (!ok) {
            fprintf(stderr, "mklfs: read error in %s\n", path);
            rc = -1;
            continue;
        }
        printf("%-24s %u bytes\n", path, (unsigned)info.size);
    }
    lfs_dir_close(&lfs, &dir);
    return rc;
}

static int verify(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mklfs -v <image>\n");
        return 1;
    }

    FILE *in = fopen(argv[2], "rb");
    if (!in) {
        fprintf(stderr, "mklfs: cannot open %s\n", argv[2]);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (size <= 0 || size % BLOCK_SIZE != 0) {
        fprintf(stderr, "mklfs: bad image size %ld\n", size);
        fclose(in);
        return 1;
    }

    disk = malloc((size_t)size);
    if (!disk || fread(disk, 1, (size_t)size, in) != (size_t)size) {
        fprintf(stderr, "mklfs: cannot read %s\n", argv[2]);
        fclose(in);
        return 1;
    }
    fclose(in);

    cfg_init((lfs_size_t)(size / BLOCK_SIZE));

    if (lfs_mount(&lfs, &cfg) < 0) {
        fprintf(stderr, "mklfs: verify mount failed\n");
        free(disk);
        return 1;
    }

    int rc = dump_dir("", 0, "") < 0 ? 1 : 0;

    if (lfs_unmount(&lfs) < 0)
        rc = 1;

    free(disk);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  mklfs <image> <size> <base-dir> <name> [name...]\n"
                "  mklfs -v <image>\n");
        return 1;
    }
    if (strcmp(argv[1], "-v") == 0)
        return verify(argc, argv);
    return create(argc, argv);
}
