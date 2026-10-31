#include "paging.h"
#include "pmm.h"
#include "io.h"
#include "string.h"
#include "console.h"
#include "platform.h"

#define IDENTITY_MB 64
#define PDE_4MB     0x00000083u  /* present | rw | page size */

static uint32_t boot_pd[1024] __attribute__((aligned(PAGE_SIZE)));

void paging_init(void)
{
    memset(boot_pd, 0, sizeof(boot_pd));
    for (uint32_t i = 0; i < IDENTITY_MB / 4; i++)
        boot_pd[i] = i * 0x400000u + PDE_4MB;

    write_cr3((uint32_t)boot_pd);
    write_cr4(read_cr4() | 0x10);            /* PSE: 4 MiB pages */
    write_cr0(read_cr0() | 0x80000000u);     /* PG               */
}

uint32_t kernel_pd_phys(void)
{
    return (uint32_t)boot_pd;
}

uint32_t paging_create_as(void)
{
    uint32_t pd_phys = pmm_alloc();
    if (!pd_phys)
        return 0;
    memcpy((void *)pd_phys, boot_pd, PAGE_SIZE);
    return pd_phys;
}

void paging_destroy_as(uint32_t pd_phys)
{
    if (!pd_phys)
        return;

    uint32_t *pd = (uint32_t *)pd_phys;
    for (int i = 0; i < 1024; i++) {
        if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_PS))
            continue;
        uint32_t *pt = (uint32_t *)(pd[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) {
            if (pt[j] & PTE_PRESENT)
                pmm_free(pt[j] & ~0xFFFu);
        }
        pmm_free((uint32_t)pt);
    }
    pmm_free(pd_phys);
}

int paging_map(uint32_t pd_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags)
{
    if (pd_phys == 0 || (vaddr & 0xFFF) || (paddr & 0xFFF))
        return -1;

    uint32_t *pd = (uint32_t *)pd_phys;
    uint32_t pdi = vaddr >> 22;
    uint32_t pti = (vaddr >> 12) & 0x3FF;

    if (!(pd[pdi] & PTE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc();
        if (!pt_phys)
            return -1;
        pd[pdi] = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else if (pd[pdi] & PTE_PS) {
        return -1; /* never override the kernel identity map */
    }

    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
    pt[pti] = (paddr & ~0xFFFu) | (flags & 0xFFF) | PTE_PRESENT;
    return 0;
}

uint32_t paging_translate(uint32_t pd_phys, uint32_t vaddr)
{
    uint32_t *pd = (uint32_t *)pd_phys;
    uint32_t pde = pd[vaddr >> 22];
    if (!(pde & PTE_PRESENT))
        return 0;
    if (pde & PTE_PS)
        return (pde & 0xFFC00000u) + (vaddr & 0x3FFFFFu);
    uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
    uint32_t pte = pt[(vaddr >> 12) & 0x3FF];
    if (!(pte & PTE_PRESENT))
        return 0;
    return (pte & ~0xFFFu) + (vaddr & 0xFFFu);
}

int paging_user_ok(uint32_t pd_phys, uint32_t addr, uint32_t len)
{
    if (pd_phys == 0)
        return 0;
    if (addr < PLAT_APP_BASE || addr >= PLAT_APP_MAX)
        return 0;
    if (len == 0)
        return 1;
    if (addr + len < addr || addr + len > PLAT_APP_MAX)
        return 0;

    uint32_t *pd = (uint32_t *)pd_phys;
    uint32_t first = addr & ~0xFFFu;
    uint32_t last = (addr + len - 1) & ~0xFFFu;
    for (uint32_t v = first;; v += PAGE_SIZE) {
        uint32_t pde = pd[v >> 22];
        if (!(pde & PTE_PRESENT) || !(pde & PTE_USER))
            return 0;
        if (!(pde & PTE_PS)) {
            uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
            uint32_t pte = pt[(v >> 12) & 0x3FF];
            if (!(pte & PTE_PRESENT) || !(pte & PTE_USER))
                return 0;
        }
        if (v == last)
            break;
    }
    return 1;
}

void paging_write(uint32_t pd_phys, uint32_t vaddr, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint32_t phys = paging_translate(pd_phys, vaddr);
        if (!phys)
            return;
        uint32_t chunk = PAGE_SIZE - (vaddr & 0xFFF);
        if (chunk > len)
            chunk = len;
        memcpy((void *)phys, s, chunk);
        vaddr += chunk;
        s += chunk;
        len -= chunk;
    }
}

void paging_zero(uint32_t pd_phys, uint32_t vaddr, uint32_t len)
{
    while (len) {
        uint32_t phys = paging_translate(pd_phys, vaddr);
        if (!phys)
            return;
        uint32_t chunk = PAGE_SIZE - (vaddr & 0xFFF);
        if (chunk > len)
            chunk = len;
        memset((void *)phys, 0, chunk);
        vaddr += chunk;
        len -= chunk;
    }
}
