#ifndef TINYOS_PAGING_H
#define TINYOS_PAGING_H

#include "types.h"

#define PTE_PRESENT 0x1
#define PTE_WRITE   0x2
#define PTE_USER    0x4
#define PTE_PS      0x80

#define USER_CODE_BASE  0x08048000u
#define USER_STACK_TOP  0x10000000u
#define USER_HEAP_BASE  0x20000000u
#define USER_ADDR_LIMIT 0x30000000u

void     paging_init(void);
uint32_t paging_create_as(void);
void     paging_destroy_as(uint32_t pd_phys);
int      paging_map(uint32_t pd_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags);
uint32_t paging_translate(uint32_t pd_phys, uint32_t vaddr);
int      paging_user_ok(uint32_t pd_phys, uint32_t addr, uint32_t len);
void     paging_write(uint32_t pd_phys, uint32_t vaddr, const void *src, uint32_t len);
void     paging_zero(uint32_t pd_phys, uint32_t vaddr, uint32_t len);
uint32_t kernel_pd_phys(void);

#endif
