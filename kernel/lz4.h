#ifndef TINYOS_LZ4_H
#define TINYOS_LZ4_H

#include "types.h"

/* Minimal LZ4 block decoder (no frame format, no checksums).

   `src` holds a raw LZ4 block as produced by LZ4_compress_default();
   `dst_cap` bounds the output.  Returns the number of bytes written, or
   a negative value on error. */
int lz4_decompress_block(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap);

/* Compressed file envelope written by the build tool:
     offset 0  : magic "LZ4TOS"  (6 bytes)
     offset 6  : u16 flags (0)
     offset 8  : u32 original_size
     offset 12 : u32 compressed_size
     offset 16 : LZ4 block data
   Returns 1 and fills *out_size when `data` is such an envelope, else 0. */
int lz4_wrap_info(const uint8_t *data, uint32_t size, uint32_t *out_size);

#define LZ4_WRAP_MAGIC   "LZ4TOS"
#define LZ4_WRAP_HEADER  16

#endif
