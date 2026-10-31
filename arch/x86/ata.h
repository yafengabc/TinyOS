#ifndef TINYOS_ATA_H
#define TINYOS_ATA_H

#include "types.h"

/* Minimal ATA PIO driver (28-bit LBA, polling, no IRQ).
   The IDE controller on the QEMU pc-i440fx machine is a PIIX3/PIIX4 at the
   classic ports: primary 0x1F0, secondary 0x170, control 0x3F6/0x376. */

#define ATA_PRIMARY_IO    0x1F0
#define ATA_SECONDARY_IO  0x170

/* Scan both buses for the first ATA drive and remember it. */
void     ata_init(void);
int      ata_present(void);
uint16_t ata_io_base(void);
int      ata_drive_no(void);

/* Read/write `count` 512-byte sectors starting at 28-bit `lba`.
   Returns 0 on success, -1 on error.  `buf` must hold count*512 bytes. */
int ata_read_sectors(uint16_t io, int drive, uint32_t lba, uint16_t count, void *buf);
int ata_write_sectors(uint16_t io, int drive, uint32_t lba, uint16_t count, const void *buf);

#endif
