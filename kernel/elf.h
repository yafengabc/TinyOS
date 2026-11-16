#ifndef TINYOS_ELF_H
#define TINYOS_ELF_H

#include "types.h"

/* Load a user program image that was decompressed directly into its task
 * window.
 *
 * `win` initially holds the raw ELF file as one linear byte stream; this
 * call slides every PT_LOAD segment to `p_vaddr - link_lo`, clears .bss and
 * applies the R_* relocations found in .rel/.rela.dyn.  The position
 * independent (ET_DYN) image is relocated to `load_base` and `entry_out`
 * receives the absolute entry address.  `win_size` bounds every placement,
 * and every write is also checked against that same window, so a corrupt
 * image can never touch the kernel.  Both the image and its bounds are
 * per-call, so two cores may load different programs at the same time.
 *
 * Returns 0 on success, -1 on error. */
int elf_load(uint8_t *win, uint32_t size, uint32_t win_size,
             uint32_t load_base, uint32_t *entry_out);

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

#endif
