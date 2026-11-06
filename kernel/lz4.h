#ifndef TINYOS_LZ4_H
#define TINYOS_LZ4_H

#include "types.h"

/* Minimal LZ4 block decoder (no frame format, no checksums).

   `src` holds a raw LZ4 block as produced by LZ4_compress_default();
   `dst_cap` bounds the output.  Returns the number of bytes written, or
   a negative value on error. */
int lz4_decompress_block(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap);

/* Compressed file envelope written by tools/mklfs.c:
     offset 0  : magic "LZ4TOS"  (6 bytes)
     offset 6  : u16 flags; bit0 = a window hint follows
     offset 8  : u32 original_size
     offset 12 : u32 compressed_size
     offset 16 : [bit0 only] u32 span, u32 stack_ask, u32 heap_ask
     offset 16 or 28 : LZ4 block data

   The hint carries the three numbers exec() needs to size a task window.
   mklfs derives them from the ELF the same way kernel/elf.c does: span is
   the page-rounded PT_LOAD p_vaddr..p_vaddr+p_memsz range, the two asks
   come from .note.tos.  They are deliberately target-independent - the
   kernel still applies its own default stack, guard and grain - so one
   image stays loadable on every board.  flags=0 keeps the old 16-byte
   shape readable, which is what a stale filesystem image looks like. */
typedef struct lz4_window_hint {
    uint32_t span;
    uint32_t stack_ask;   /* 0 means "use the board default" */
    uint32_t heap_ask;    /* 0 means the program asked for nothing */
} lz4_window_hint_t;

/* Returns the byte offset of the LZ4 block, or 0 if `data` is not a valid
   envelope.  *out_size gets original_size.  When the hint is present
   *has_hint is 1 and *hint is filled; otherwise *has_hint is 0. */
uint32_t lz4_wrap_info(const uint8_t *data, uint32_t size,
                       uint32_t *out_size,
                       lz4_window_hint_t *hint, int *has_hint);

#define LZ4_WRAP_MAGIC      "LZ4TOS"
#define LZ4_WRAP_HEADER     16
#define LZ4_WRAP_HINT_BIT   0x0001u
#define LZ4_WRAP_HINT_SIZE  12
#define LZ4_WRAP_HEADER_V1  28

#endif
