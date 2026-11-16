#ifndef TINYOS_EXEC_FORMAT_H
#define TINYOS_EXEC_FORMAT_H

#include <stdint.h>

#include "lz4.h"

/* C2/A-3 seam: a container format (how a file becomes a raw image in the
 * window) and an image format (how that raw image becomes segments).  The
 * current tree has one of each - LZ4TOS and ELF32 - but exec() dispatches
 * through this table instead of naming either implementation directly.
 *
 * probe() returns the container header offset, or 0 when this handler does
 * not recognise the file.  load() must never write outside raw_size. */
struct exec_format {
    const char *name;
    uint32_t (*probe)(const char *path, uint32_t *raw_size,
                      lz4_window_hint_t *hint);
    int (*load)(const char *path, uint32_t header, uint8_t *dst,
                uint32_t raw_size);
    const char *note;
};

struct exec_image_format {
    const char *name;
    int (*probe)(const uint8_t *data, uint32_t size, uint32_t *lo,
                 uint32_t *hi, uint32_t *entry, uint32_t *writable_from);
    int (*load)(uint8_t *win, uint32_t size, uint32_t win_size,
                uint32_t load_base, uint32_t *entry_out);
    const char *note;
};

#endif
