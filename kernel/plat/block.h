#ifndef TINYOS_PLAT_BLOCK_H
#define TINYOS_PLAT_BLOCK_H

#include "types.h"

/* ---- block device (backing store for littlefs) ----------------------- */

/* First block of the littlefs partition.  The PC port writes the
 * filesystem right after the kernel image on the boot disk (181); boards
 * that embed the image in RAM start at 0. */
extern const uint32_t PLAT_LFS_LBA;

/* Number of littlefs blocks in the partition.  Board-specific (each
 * board's layout.c sets it) so the C3 can grow its 4 MiB flash partition
 * without dragging the other boards up with it.  The kernel mounts this
 * many blocks of FS_BLOCK_SIZE starting at PLAT_LFS_LBA. */
extern const uint32_t PLAT_LFS_BLOCKS;

int      plat_block_read(uint32_t lba, uint32_t count, void *buf);
int      plat_block_write(uint32_t lba, uint32_t count, const void *buf);
int      plat_block_erase(uint32_t lba, uint32_t count);
                                     /* make count sectors read the erased
                                      * value: a real NOR erase where the
                                      * medium needs one, an 0xFF fill
                                      * where plain writes are safe */
uint32_t plat_block_size(void);            /* bytes per sector */
uint32_t plat_block_count(void);

/* Capacity of the primary flash in bytes, as the port can detect it at
 * runtime; 0 when the port cannot tell (the board's configured size is then
 * the only answer).  Implemented per chip; a weak default returns 0. */
uint32_t plat_flash_detected_bytes(void);

/* 8-byte unique id from the flash part itself, for the USB serial string.
 * RP2350 gets this from SYS_INFO, but RP2040 has no such register: its W25Qxx
 * answers command 0x4B.  A port that cannot read it returns 0 and the caller
 * falls back to a constant; a weak default says "not available". */
int plat_flash_unique_id(uint8_t out[8]);

/* Effective filesystem size in blocks: the detected capacity clamped to the
 * board's configured flash_bytes, minus the partition offset.  Generic: it
 * only needs plat_flash_detected_bytes(). */
uint32_t plat_lfs_blocks(void);
void plat_flash_probe_debug(uint32_t *out);
uint32_t plat_usb_poll_ns(void);

/* Console UART in twelve words, for /proc/uart0.  A port that can lose its
 * TX side without losing its RX side (the RP2350 after a BOOTSEL flash does
 * exactly that) needs its programming - CR, the divider, the pad and the pin
 * mux - readable from the *other* console, or the only way to tell "the
 * board stopped transmitting" from "the adapter stopped receiving" is to
 * guess.  All zero means the port has nothing to say. */
void plat_uart_debug(uint32_t *out);

/* Make the console UART transmit, and watch the TX pad's own input buffer
 * while it does.  This is the only way a board can answer "am I still
 * driving the wire?" about itself: the register programming can be perfect
 * (UARTEN|TXE|RXE, correct divisor, pad not isolated, pin mux on UART) and
 * the line can still be dead, and the adapter at the other end cannot tell
 * the two apart.  Fills out[0]=samples the pad read low, out[1]=high,
 * out[2]=transitions, out[3]=FR once the last byte has shifted out,
 * out[4]/out[5]=the same two counts taken *before* the first byte (an idle
 * UART must read high there), out[6]=high count of a control pin driven by
 * the far end (the adapter's TX), out[7]=high count of the LED pin.
 * Returns 1 when the probe ran, 0 when the port has no such pin readback. */
int plat_uart_tx_probe(uint32_t *out);

/* Drive the console UART's TX pad from the GPIO/SIO peripheral instead and
 * read it back, which separates "the pad cannot pull the line high" from
 * "the UART's output is not reaching the pad".  out[0..1]=samples read high
 * and low while driving 1, out[2..3]=the same while driving 0.  Returns 1
 * when it ran, 0 when the port cannot do it. */
int plat_gpio_drive_probe(uint32_t *out);

/* Boot-progress stage, called from kmain() between its steps.  `n` is a small
 * number chosen by the caller; a port with a surviving boot log records it,
 * everything else ignores it (weak no-op in kernel/main.c).  It exists because
 * a port's own markers stop at the end of its early boot, and the failures
 * that matter ("the console works but the shell never speaks") happen after
 * that. */
void plat_boot_stage(uint32_t n);

#endif /* TINYOS_PLAT_BLOCK_H */
