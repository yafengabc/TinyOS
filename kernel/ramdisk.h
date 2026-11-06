#ifndef TINYOS_RAMDISK_H
#define TINYOS_RAMDISK_H

#include "types.h"

/* The filesystem is a littlefs partition that lives on the platform's
   block device: on the PC it is written to the disk right after the kernel
   image (LBA 181), on qemu-virt it is linked into the kernel as RAM.  The
   first block of the partition is PLAT_LFS_LBA, supplied by the board (see
   kernel/platform.h).  The API below keeps the old ramdisk shape so the
   rest of the kernel (and the user ABI) is unchanged. */

void ramdisk_init(void);
int  ramdisk_ok(void);
/* Directory-aware listing: count of entries in `dirpath` (absolute), and the
 * index-th bare entry name copied into `buf` (cap bytes).  The copy is what
 * makes this safe on SMP: the names live in a shared snapshot, so handing out
 * a pointer would go stale the moment the other CPU enumerated something.
 * Returns the name length, 0 past the last entry, -1 if it does not fit. */
int      ramdisk_list_count(const char *dirpath);
int      ramdisk_list_name(const char *dirpath, uint32_t index, char *buf,
                           int cap);
int      ramdisk_stat(const char *path, uint32_t *size, uint8_t *type);
/* Remove a file.  Directories are refused: an empty directory would need a
 * second syscall (rmdir) to mean anything, and nothing in the system creates
 * one at runtime. */
int      ramdisk_unlink(const char *path);
int      ramdisk_df(uint32_t *total, uint32_t *used);

/* Block-device call counters, exposed through /proc/lfs.  Profiling aid:
 * comparing these against the number of bytes a command wrote says where the
 * time goes.  On the C3, writing 3 MiB is 5,934 progs of 512 bytes and 743
 * block erases - the erases alone are about half the wall clock, which is
 * why coalescing writes further bought nothing. */
struct lfs_io_stats {
    uint32_t reads;          /* bd_read calls          */
    uint32_t read_sectors;   /* sectors those read     */
    uint32_t progs;          /* bd_prog calls          */
    uint32_t prog_bytes;     /* bytes written through  */
    uint32_t erases;         /* block erases           */
};
void     ramdisk_io_stats(struct lfs_io_stats *s);

/* Stream-style access used by the fd layer (open/read/close). */
int      ramdisk_open(const char *path);   /* returns handle, -1 on error */
int      ramdisk_open_rw(const char *path);/* read-write, no truncation  */
int      ramdisk_create(const char *path); /* create/truncate for writing */
uint32_t ramdisk_size(int h);
int      ramdisk_read(int h, void *buf, uint32_t len);
int      ramdisk_write(int h, const void *buf, uint32_t len);
int      ramdisk_seek(int h, uint32_t pos);
void     ramdisk_close(int h);

#endif
