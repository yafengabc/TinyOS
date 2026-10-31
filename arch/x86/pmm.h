#ifndef TINYOS_PMM_H
#define TINYOS_PMM_H

#include "types.h"

#define PAGE_SIZE   4096
#define PHYS_START  0x00400000u  /* first 4 MiB are identity mapped and reserved */
#define PHYS_LIMIT  0x04000000u  /* we manage the first 64 MiB of RAM        */

void     pmm_init(void);
uint32_t pmm_alloc(void);
void     pmm_free(uint32_t addr);
uint32_t pmm_free_bytes(void);

#endif
