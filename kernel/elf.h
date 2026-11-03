#ifndef TINYOS_ELF_H
#define TINYOS_ELF_H

#include "types.h"

/* Load a user program image.
 *
 * The loader accepts both ET_EXEC (fixed address) and ET_DYN (position
 * independent).  For ET_DYN the image is relocated to `load_base`; the
 * segment contents and any R_* relocations found in .rela.dyn are adjusted
 * accordingly.  `entry_out` receives the absolute entry address.
 *
 * Every write is bounds-checked against the window selected with
 * elf_set_window(), so a corrupt image can never touch the kernel.
 *
 * Returns 0 on success, -1 on error. */
int elf_load(const uint8_t *data, uint32_t size, uint32_t load_base,
             uint32_t *entry_out);

/* Same, but reports the lowest and highest addresses the image touches,
 * plus the lowest address of any writable segment (the code/data split the
 * hardware needs).  Callers use this to reserve or check the region
 * beforehand. */
int elf_probe(const uint8_t *data, uint32_t size, uint32_t *load_base_out,
              uint32_t *mem_end_out, uint32_t *entry_out,
              uint32_t *writable_from_out);

/* What the program asks of the window, read from the notes its image
 * carries (user/lib.h's TOS_STACK() and TOS_HEAP(), placed in .note.tos and
 * linked into a PT_NOTE segment by user/user.ld).  Either *out is 0 when
 * the image says nothing about it: stack 0 means the board default, heap 0
 * means "whatever the window has left".  Both are requests, not facts -
 * process.c validates them before the window is sized.  Returns -1 if the
 * image is not a readable ELF32; callers that got this far already know
 * better. */
int elf_tos_note(const uint8_t *data, uint32_t size,
                 uint32_t *stack_out, uint32_t *heap_out);

/* Select the task window [lo, hi) the loader may write into.  process.c
 * sets this before every load; the loader never writes outside it. */
void elf_set_window(uint32_t lo, uint32_t hi);

#endif
