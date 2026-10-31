#include "elf.h"
#include "platform.h"
#include "string.h"
#include "console.h"

/* Task window the loader may write into (set by process.c before every
 * load).  The flat model has no page tables, so "mapping" is a no-op and
 * every write is a plain store. */
static uint32_t g_win_lo;
static uint32_t g_win_hi;

void elf_set_window(uint32_t lo, uint32_t hi)
{
    g_win_lo = lo;
    g_win_hi = hi;
}

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PF_X       1
#define PF_W       2
#define EM_386     3
#define EM_RISCV   243
#define ELFCLASS32 1

#define ET_EXEC 2
#define ET_DYN  3

/* RISC-V relocation types */
#define R_RISCV_NONE       0
#define R_RISCV_32         1
#define R_RISCV_RELATIVE   3
#define R_RISCV_JUMP_SLOT  5

/* x86-32 relocation types */
#define R_386_NONE         0
#define R_386_32           1
#define R_386_JMP_SLOT     7
#define R_386_RELATIVE     8

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

typedef struct {
    uint32_t d_tag;
    uint32_t d_val;
} __attribute__((packed)) Elf32_Dyn;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
} __attribute__((packed)) Elf32_Rela;

#define DT_NULL     0
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_REL      17
#define DT_RELSZ    18

#define R_SYM(info)  ((info) >> 8)
#define R_TYPE(info) ((info) & 0xFF)

/* ------------------------------------------------------------------ */
/* writes are plain stores: the flat model has no address translation  */
/* ------------------------------------------------------------------ */

/* The loader may only touch the task window selected with elf_set_window().
 * Without an MMU a corrupt or hostile image could otherwise overwrite the
 * kernel through p_vaddr / r_offset. */
static int user_range_ok(uint32_t va, uint32_t len)
{
    if (len == 0)
        return 1;
    if (va < g_win_lo)
        return 0;
    if (va + len < va) /* overflow */
        return 0;
    return va + len <= g_win_hi;
}

static int copy_to_user(uint32_t va, const void *src, uint32_t len)
{
    if (!user_range_ok(va, len))
        return -1;
    memcpy((void *)va, src, len);
    return 0;
}

static int zero_user(uint32_t va, uint32_t len)
{
    if (!user_range_ok(va, len))
        return -1;
    memset((void *)va, 0, len);
    return 0;
}

static int read_u32(uint32_t va, uint32_t *out)
{
    if (!user_range_ok(va, 4))
        return -1;
    memcpy(out, (const void *)va, 4);
    return 0;
}

static int write_u32(uint32_t va, uint32_t val)
{
    if (!user_range_ok(va, 4))
        return -1;
    memcpy((void *)va, &val, 4);
    return 0;
}

/* ------------------------------------------------------------------ */

static const Elf32_Phdr *phdr_at(const uint8_t *data, const Elf32_Ehdr *eh, uint32_t i)
{
    return (const Elf32_Phdr *)(data + eh->e_phoff +
                                (uint32_t)i * sizeof(Elf32_Phdr));
}

static int header_ok(const uint8_t *data, uint32_t size, const Elf32_Ehdr **out)
{
    if (size < sizeof(Elf32_Ehdr))
        return -1;

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)data;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return -1;
    if (eh->e_ident[4] != ELFCLASS32)
        return -1;
    if (eh->e_phentsize != sizeof(Elf32_Phdr))
        return -1;
    if (eh->e_phnum == 0)
        return -1;

    uint32_t ph_end = eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(Elf32_Phdr);
    if (ph_end > size)
        return -1;

    *out = eh;
    return 0;
}

int elf_probe(const uint8_t *data, uint32_t size, uint32_t *load_base_out,
              uint32_t *mem_end_out, uint32_t *entry_out,
              uint32_t *writable_from_out)
{
    const Elf32_Ehdr *eh;
    if (header_ok(data, size, &eh) != 0)
        return -1;

    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    uint32_t wlo = 0xFFFFFFFFu;
    int entry_in_code = 0;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = phdr_at(data, eh, i);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_vaddr < lo)
            lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > hi)
            hi = ph->p_vaddr + ph->p_memsz;
        if ((ph->p_flags & PF_W) && ph->p_vaddr < wlo)
            wlo = ph->p_vaddr;
        if (ph->p_flags & PF_X) {
            /* The loader splits the window into one read-execute band and
               one read-write band, so a segment asking for both cannot be
               honoured: reject it rather than quietly pick one. */
            if (ph->p_flags & PF_W) {
                kprintf("elf: segment %u is both writable and executable\n", i);
                return -1;
            }
            if (eh->e_entry >= ph->p_vaddr &&
                eh->e_entry < ph->p_vaddr + ph->p_memsz)
                entry_in_code = 1;
        }
    }
    if (lo == 0xFFFFFFFFu || hi <= lo)
        return -1;
    if (!entry_in_code) {
        kprintf("elf: entry 0x%x is not in an executable segment\n", eh->e_entry);
        return -1;
    }

    if (load_base_out)
        *load_base_out = lo;
    if (mem_end_out)
        *mem_end_out = hi;
    if (entry_out)
        *entry_out = eh->e_entry;
    /* Everything below the first writable segment is code, everything from
       there up may be written.  That is the split the loader hands to the
       hardware: an image without a writable segment has no data at all.
       This assumes an image whose writable segments follow its code, which
       is what user/user.ld produces (text, rodata, data, bss); an image
       with data first would put its code in the writable band and die on
       the first fetch. */
    if (writable_from_out)
        *writable_from_out = (wlo == 0xFFFFFFFFu) ? hi : wlo;
    return 0;
}

/* Convert a virtual address inside the image to an offset in `data`.
 * Program headers give the vaddr -> file offset mapping for PT_LOAD
 * segments; the dynamic table lives inside one of them. */
static int vaddr_to_off(const uint8_t *data, const Elf32_Ehdr *eh, uint32_t va,
                        uint32_t *off_out)
{
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = phdr_at(data, eh, i);
        if (ph->p_type != PT_LOAD)
            continue;
        if (va >= ph->p_vaddr && va < ph->p_vaddr + ph->p_filesz) {
            *off_out = ph->p_offset + (va - ph->p_vaddr);
            return 0;
        }
    }
    return -1;
}

/* Apply the image's relocation entries.  `bias` = actual_base - link_base. */
static int apply_relocations(const uint8_t *data, uint32_t size,
                             const Elf32_Ehdr *eh, uint32_t bias)
{
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = phdr_at(data, eh, i);
        if (ph->p_type != PT_DYNAMIC)
            continue;
        if (ph->p_offset + ph->p_filesz > size)
            return -1;

        const Elf32_Dyn *dyn = (const Elf32_Dyn *)(data + ph->p_offset);
        uint32_t ndyn = ph->p_filesz / sizeof(Elf32_Dyn);

        uint32_t rela = 0, relasz = 0, relaent = sizeof(Elf32_Rela);
        uint32_t rel = 0, relsz = 0, relent = 8;

        for (uint32_t d = 0; d < ndyn; d++) {
            if (dyn[d].d_tag == DT_NULL)
                break;
            switch (dyn[d].d_tag) {
            case DT_RELA:    rela = dyn[d].d_val; break;
            case DT_RELASZ:  relasz = dyn[d].d_val; break;
            case DT_RELAENT: relaent = dyn[d].d_val; break;
            case DT_REL:     rel = dyn[d].d_val; break;
            case DT_RELSZ:   relsz = dyn[d].d_val; break;
            default: break;
            }
        }
        if (relaent == 0)
            relaent = sizeof(Elf32_Rela);
        if (relent == 0)
            relent = 8;

        /* dt_val holds a *virtual address*; convert to a file offset. */
        uint32_t rela_off = 0, rel_off = 0;
        if (rela && vaddr_to_off(data, eh, rela, &rela_off) != 0) {
            kprintf("elf: DT_RELA at 0x%x not mapped\n", rela);
            return -1;
        }
        if (rel && vaddr_to_off(data, eh, rel, &rel_off) != 0) {
            kprintf("elf: DT_REL at 0x%x not mapped\n", rel);
            return -1;
        }

        /* .rela.dyn (explicit addend) */
        if (rela && relasz) {
            if (rela_off + relasz > size)
                return -1;
            for (uint32_t o = 0; o + sizeof(Elf32_Rela) <= relasz; o += relaent) {
                Elf32_Rela r;
                memcpy(&r, data + rela_off + o, sizeof(r));

                uint32_t type = R_TYPE(r.r_info);
                if (type == R_RISCV_NONE || type == R_386_NONE)
                    continue;

                uint32_t where = r.r_offset + bias;
                uint32_t val;

                /* RELA carries the addend explicitly; the value stored at
                   the target is meaningless (unlike REL).  With no dynamic
                   symbol table, symbols stay unresolved (S = 0), so every
                   supported type reduces to addend + load bias. */

                switch (type) {
                case R_RISCV_RELATIVE:
                case R_386_RELATIVE:
                case R_RISCV_32:      /* same numeric value as R_386_32 */
                case R_RISCV_JUMP_SLOT:
                case R_386_JMP_SLOT:
                    val = (uint32_t)r.r_addend + bias;
                    break;
                default:
                    kprintf("exec: unsupported relocation %u\n", type);
                    return -1;
                }

                if (write_u32(where, val) != 0)
                    return -1;
            }
        }

        /* .rel (implicit addend) */
        if (rel && relsz) {
            if (rel_off + relsz > size) {
                kprintf("elf: .rel past end (off=0x%x sz=0x%x size=0x%x)\n",
                        rel_off, relsz, size);
                return -1;
            }
            for (uint32_t o = 0; o + 8 <= relsz; o += relent) {
                uint32_t r_offset, r_info;
                memcpy(&r_offset, data + rel_off + o, 4);
                memcpy(&r_info, data + rel_off + o + 4, 4);

                uint32_t type = R_TYPE(r_info);
                if (type == R_386_NONE)
                    continue;

                uint32_t where = r_offset + bias;
                uint32_t val;
                if (read_u32(where, &val) != 0) {
                    kprintf("elf: reloc read failed at 0x%x (r_offset=0x%x bias=0x%x type=%u)\n",
                            where, r_offset, bias, type);
                    return -1;
                }

                switch (type) {
                case R_386_RELATIVE:
                case R_386_32:
                case R_386_JMP_SLOT:
                    val += bias;
                    break;
                default:
                    kprintf("exec: unsupported relocation %u\n", type);
                    return -1;
                }
                if (write_u32(where, val) != 0) {
                    kprintf("elf: reloc write failed at 0x%x\n", where);
                    return -1;
                }
            }
        }
        return 0;
    }
    return 0; /* no PT_DYNAMIC: nothing to relocate */
}

int elf_load(const uint8_t *data, uint32_t size, uint32_t load_base,
             uint32_t *entry_out)
{
    const Elf32_Ehdr *eh;
    if (header_ok(data, size, &eh) != 0)
        return -1;

#if defined(__riscv)
    const uint16_t expected_machine = EM_RISCV;
#elif defined(__i386__)
    const uint16_t expected_machine = EM_386;
#else
    const uint16_t expected_machine = eh->e_machine;
#endif
    if (eh->e_machine != expected_machine)
        return -1;

    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return -1;

    /* Work out how to map link-time addresses to real ones.
     *
     * ET_EXEC: the image must run where it was linked -> bias 0.
     * ET_DYN : the image is linked at 0, so a link-time address A must be
     *          placed at load_base + A.  Relocation r_offset values follow
     *          the same rule.  bias is the additive constant for *addresses*
     *          stored inside the image (which are also 0-based). */
    uint32_t link_lo = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = phdr_at(data, eh, i);
        if (ph->p_type == PT_LOAD && ph->p_vaddr < link_lo)
            link_lo = ph->p_vaddr;
    }
    if (link_lo == 0xFFFFFFFFu)
        return -1;

    uint32_t bias = 0;
    if (eh->e_type == ET_DYN)
        bias = load_base - link_lo;

    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = phdr_at(data, eh, i);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_offset + ph->p_filesz > size) {
            kprintf("elf: segment %u out of file\n", i);
            return -1;
        }
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
            kprintf("elf: segment %u wraps\n", i);
            return -1;
        }

        uint32_t dst = ph->p_vaddr + bias;
        if (!user_range_ok(dst, ph->p_memsz)) {
            kprintf("elf: segment %u outside app window (0x%x+0x%x)\n",
                    i, dst, ph->p_memsz);
            return -1;
        }
        if (copy_to_user(dst, data + ph->p_offset, ph->p_filesz) != 0)
            return -1;
        if (ph->p_memsz > ph->p_filesz) {
            if (zero_user(dst + ph->p_filesz, ph->p_memsz - ph->p_filesz) != 0)
                return -1;
        }
    }

    if (apply_relocations(data, size, eh, bias) != 0)
        return -1;

    uint32_t entry = eh->e_entry + bias;
    if (!user_range_ok(entry, 1)) {
        kprintf("elf: entry 0x%x outside app window\n", entry);
        return -1;
    }
    *entry_out = entry;
    return 0;
}
