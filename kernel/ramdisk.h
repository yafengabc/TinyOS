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
uint32_t ramdisk_count(void);
int  ramdisk_find(const char *name, uint8_t **data, uint32_t *size);
void ramdisk_release(void *data);
const char *ramdisk_name(uint32_t index, uint32_t *size);
/* Directory-aware listing: count/name of entries in `dirpath` (absolute).
 * Used by SYS_READDIR now that it lists the current working directory. */
int      ramdisk_list_count(const char *dirpath);
const char *ramdisk_list_name(const char *dirpath, uint32_t index, uint32_t *size);
int      ramdisk_stat(const char *path, uint32_t *size, uint8_t *type);
int      ramdisk_df(uint32_t *total, uint32_t *used);

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
