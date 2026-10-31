#ifndef TINYOS_RAMDISK_H
#define TINYOS_RAMDISK_H

#include "types.h"

/* The filesystem is a littlefs partition on the ATA disk.  The build tool
   writes it right after the kernel image, so its first sector LBA is
   BootSectors + KernelSectors (1 + 180 = 181).  Keep this in sync with
   internal/build/build.go.  The API below keeps the old ramdisk shape so
   the rest of the kernel (and the user ABI) is unchanged. */

#define LFS_PARTITION_LBA 181

void ramdisk_init(void);
int  ramdisk_ok(void);
uint32_t ramdisk_count(void);
int  ramdisk_find(const char *name, uint8_t **data, uint32_t *size);
void ramdisk_release(void *data);
const char *ramdisk_name(uint32_t index, uint32_t *size);

/* Stream-style access used by the fd layer (open/read/close). */
int      ramdisk_open(const char *path);   /* returns handle, -1 on error */
int      ramdisk_create(const char *path); /* create/truncate for writing */
uint32_t ramdisk_size(int h);
int      ramdisk_read(int h, void *buf, uint32_t len);
int      ramdisk_write(int h, const void *buf, uint32_t len);
void     ramdisk_close(int h);

#endif
