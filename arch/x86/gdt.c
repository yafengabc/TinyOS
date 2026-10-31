#include "gdt.h"
#include "string.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0, ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt_entries[6];
static struct gdt_ptr   gdt_ptr;
static struct tss_entry tss;

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt_entries[i].base_low  = (uint16_t)(base & 0xFFFF);
    gdt_entries[i].base_mid  = (uint8_t)((base >> 16) & 0xFF);
    gdt_entries[i].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt_entries[i].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt_entries[i].gran      = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt_entries[i].access    = access;
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

void gdt_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = KERNEL_DS;
    tss.iomap_base = (uint16_t)sizeof(tss);

    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0x000FFFFF, 0x9A, 0xCF); /* kernel code */
    gdt_set(2, 0, 0x000FFFFF, 0x92, 0xCF); /* kernel data */
    gdt_set(3, 0, 0x000FFFFF, 0xFA, 0xCF); /* user code   */
    gdt_set(4, 0, 0x000FFFFF, 0xF2, 0xCF); /* user data   */
    gdt_set(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdt_ptr.limit = (uint16_t)(sizeof(gdt_entries) - 1);
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
    __asm__ volatile(
        "ljmp $0x08, $1f\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        :
        :
        : "eax", "memory");

    uint16_t sel = TSS_SEL;
    __asm__ volatile("ltr %0" : : "r"(sel));
}
