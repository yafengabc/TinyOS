#include "ata.h"
#include "io.h"
#include "console.h"

/* Status register bits. */
#define ATA_ST_BSY 0x80
#define ATA_ST_DRDY 0x40
#define ATA_ST_DRQ 0x08
#define ATA_ST_ERR 0x01

/* We poll the status register instead of using IRQ14: a busy-wait is much
   simpler to get right in a single-tasking kernel, and PIO transfers to a
   small SSD-sized image in QEMU complete in microseconds. */

static uint16_t active_io;
static int      active_drive;

/* Wait until BSY clears.  Returns 0 on success, -1 if the drive reported an
   error or never finished (bounded by the loop). */
static int wait_bsy_clear(uint16_t io)
{
    for (int i = 0; i < 10000000; i++) {
        uint8_t st = inb(io + 7);
        if (!(st & ATA_ST_BSY))
            return (st & ATA_ST_ERR) ? -1 : 0;
    }
    return -1;
}

/* Wait until BSY is clear and DRQ (data ready) is set. */
static int wait_drq(uint16_t io)
{
    for (int i = 0; i < 10000000; i++) {
        uint8_t st = inb(io + 7);
        if (st & ATA_ST_BSY)
            continue;
        if (st & ATA_ST_ERR)
            return -1;
        if (st & ATA_ST_DRQ)
            return 0;
    }
    return -1;
}

static void select_drive(uint16_t io, int drive, uint32_t lba)
{
    /* Device/Head register: LBA mode + drive select + LBA bits 24-27. */
    outb(io + 6, (uint8_t)(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)));
    io_wait();
}

static void set_lba(uint16_t io, uint32_t lba, uint16_t count)
{
    /* 28-bit LBA register map:
         io+2 sector count | io+3 LBA low | io+4 LBA mid | io+5 LBA high
       io+6 (device/head) was set by select_drive and must not be touched
       here or the drive-select / LBA bits 24-27 are lost. */
    outb(io + 2, (uint8_t)count);        /* sector count */
    outb(io + 3, (uint8_t)lba);          /* LBA low  */
    outb(io + 4, (uint8_t)(lba >> 8));   /* LBA mid  */
    outb(io + 5, (uint8_t)(lba >> 16));  /* LBA high */
}

int ata_read_sectors(uint16_t io, int drive, uint32_t lba, uint16_t count, void *buf)
{
    if (count == 0 || wait_bsy_clear(io) != 0)
        return -1;

    select_drive(io, drive, lba);
    set_lba(io, lba, count);
    outb(io + 7, 0x20);                    /* READ SECTORS */

    uint16_t *p = buf;
    for (uint16_t s = 0; s < count; s++) {
        if (wait_drq(io) != 0)
            return -1;
        for (int i = 0; i < 256; i++)      /* one sector = 256 words */
            p[s * 256 + i] = inw(io);
        if (wait_bsy_clear(io) != 0)
            return -1;
    }
    return 0;
}

int ata_write_sectors(uint16_t io, int drive, uint32_t lba, uint16_t count, const void *buf)
{
    if (count == 0 || wait_bsy_clear(io) != 0)
        return -1;

    select_drive(io, drive, lba);
    set_lba(io, lba, count);
    outb(io + 7, 0x30);                    /* WRITE SECTORS */

    const uint16_t *p = buf;
    for (uint16_t s = 0; s < count; s++) {
        if (wait_drq(io) != 0)
            return -1;
        for (int i = 0; i < 256; i++)
            outw(io, p[s * 256 + i]);
        io_wait();
        if (wait_bsy_clear(io) != 0)       /* command must finish cleanly */
            return -1;
    }
    return 0;
}

int ata_drive_present(uint16_t io, int drive)
{
    /* Select the drive and see if anything answers. */
    outb(io + 6, (uint8_t)(0xA0 | (drive << 4)));
    io_wait();
    if (inb(io + 7) == 0xFF)               /* no drive on this channel */
        return 0;
    if (wait_bsy_clear(io) != 0)
        return 0;

    /* IDENTIFY: an ATA drive returns word 0 with bit 15 clear and bit 14
       clear; ATAPI devices report 0xEB14, missing devices 0xFFFF-ish. */
    outb(io + 7, 0xEC);
    if (wait_drq(io) != 0)
        return 0;
    uint16_t w0 = inw(io);
    for (int i = 1; i < 256; i++)
        (void)inw(io);                     /* drain the 512-byte buffer */
    if (w0 & 0x8000)                       /* no device */
        return 0;
    if (w0 & 0x4000)                       /* ATAPI (0xEB14) */
        return 0;
    return 1;
}

void ata_init(void)
{
    active_io = 0;
    active_drive = 0;

    static const uint16_t ports[2] = { ATA_PRIMARY_IO, ATA_SECONDARY_IO };
    for (int b = 0; b < 2; b++) {
        for (int d = 0; d < 2; d++) {
            if (ata_drive_present(ports[b], d)) {
                active_io = ports[b];
                active_drive = d;
                kprintf("[ata] drive on port 0x%x, drive %d\n", ports[b], d);
                return;
            }
        }
    }
    kprintf("[ata] no ATA drive found\n");
}

int ata_present(void)
{
    return active_io != 0;
}

uint16_t ata_io_base(void)
{
    return active_io;
}

int ata_drive_no(void)
{
    return active_drive;
}
