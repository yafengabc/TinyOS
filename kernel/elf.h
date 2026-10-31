#ifndef TINYOS_ELF_H
#define TINYOS_ELF_H

#include "types.h"

/* Load a user program image.
 *
 * The loader accepts both ET_EXEC (fixed address, used by the x86 port) and
 * ET_DYN (position independent).  For ET_DYN the image is relocated to
 * `load_base`; the segment contents and any R_* relocations found in
 * .rela.dyn are adjusted accordingly.  `entry_out` receives the absolute
 * entry address.
 *
 * `writable` is a callback the caller supplies so the loader can write to
 * the target address space without knowing how that space is implemented
 * (identity-mapped pages on x86, plain writes on a flat RV32/MCU model).
 *
 * Returns 0 on success, -1 on error. */
int elf_load(const uint8_t *data, uint32_t size, uint32_t load_base,
             uint32_t *entry_out);

/* Same, but reports the lowest and highest addresses the image touches.
 * Callers use this to reserve or check the region beforehand. */
int elf_probe(const uint8_t *data, uint32_t size, uint32_t *load_base_out,
              uint32_t *mem_end_out, uint32_t *entry_out);

/* Select the address space the loader writes into.  Pass the process's page
 * directory (as returned by paging_create_as).  0 means "the active one". */
void elf_set_address_space(uint32_t pd);

#endif
