#ifndef TINYOS_LZ4_H
#define TINYOS_LZ4_H

#include "types.h"

/* Compressed file envelope written by tools/mklfs.c:
     offset 0  : magic "LZ4TOS"  (6 bytes)
     offset 6  : u16 flags; bit0 = a window hint follows (required)
     offset 8  : u32 original_size
     offset 12 : u32 compressed_size
     offset 16 : u32 span, u32 stack_ask, u32 heap_ask
     offset 28 : LZ4 block data

   The hint carries the three numbers exec() needs to size a task window.
   mklfs derives them from the ELF the same way kernel/elf.c does: span is
   the page-rounded PT_LOAD p_vaddr..p_vaddr+p_memsz range, the two asks
   come from .note.tos.  They are deliberately target-independent - the
   kernel still applies its own default stack, guard and grain - so one
   image stays loadable on every board.  There is no legacy 16-byte shape:
   the kernel has to know the window size before it can decompress, and a
   build that cannot derive a hint is an error in mklfs. */
typedef struct lz4_window_hint {
    uint32_t span;
    uint32_t stack_ask;   /* 0 means "use the board default" */
    uint32_t heap_ask;    /* 0 means the program asked for nothing */
} lz4_window_hint_t;

/* Returns the byte offset of the LZ4 block (always LZ4_WRAP_HEADER_V1),
   or 0 if `prefix` is not a valid hinted envelope.  `size` is the full file
   size from the filesystem; only the first LZ4_WRAP_HEADER_V1 bytes need be
   readable at `prefix`, which is all exec() reads before reserving the
   window.  *out_size gets original_size and *hint gets the three values. */
uint32_t lz4_wrap_info_prefix(const uint8_t *prefix, uint32_t prefix_len,
                              uint32_t size, uint32_t *out_size,
                              lz4_window_hint_t *hint);

/* ---- streaming block decoder ------------------------------------------
 *
 * exec() decompresses straight from the filesystem into the task window, so
 * the kernel heap never holds a copy of the compressed block or of the raw
 * ELF.  This decoder keeps a small input buffer and asks a callback to
 * refill it; LZ4 tokens, 255-run length bytes, literals and the two-byte
 * match offset may straddle two refills, which is exactly what the buffer
 * is for.
 *
 * Output must be one contiguous buffer: LZ4 matches copy from already
 * produced bytes (`dst[op+i] = dst[from+i]`), so the destination has to be
 * the final linear image - a task window qualifies, scattered segments do
 * not. */

typedef int (*lz4_stream_read_fn)(void *ctx, uint8_t *buf, uint32_t cap);
/* Returns bytes actually read (0 = EOF), or -1 on error. */

typedef struct lz4_stream {
    lz4_stream_read_fn read;
    void              *ctx;
    uint8_t            in[256];
    uint32_t           ip;
    uint32_t           ilen;
    int                eof;
} lz4_stream_t;

/* Decompress one block from `s` into `dst`, writing at most `dst_cap` bytes.
   Returns the number of bytes written, or -1 on a corrupt block, a read
   error, or an output overrun.  The caller checks the result against the
   envelope's original_size. */
int lz4_stream_decompress(lz4_stream_t *s, uint8_t *dst, uint32_t dst_cap);

#define LZ4_WRAP_MAGIC      "LZ4TOS"
#define LZ4_WRAP_HEADER     16
#define LZ4_WRAP_HINT_BIT   0x0001u
#define LZ4_WRAP_HINT_SIZE  12
#define LZ4_WRAP_HEADER_V1  28

#endif
