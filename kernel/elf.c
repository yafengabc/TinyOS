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
#define PT_NOTE    4
#define PF_X       1
#define PF_W       2
#define EM_386     3
#define EM_RISCV   243
#define EM_ARM     40
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

/* ARM (Cortex-M) relocation types.  ARM uses the .rel.dyn form (implicit
 * addend stored at the target), and with no dynamic symbol table every
 * supported type reduces to "value already in place plus load bias":
 * RELATIVE is an absolute address fixed up at load, ABS32 is the same for
 * a plain 32-bit datum, and GLOB_DAT/JUMP_SLOT point at local functions
 * (S = 0, so the stored value is the link-time address itself). */
#define R_ARM_ABS32        2
#define R_ARM_GLOB_DAT     21
#define R_ARM_JUMP_SLOT    22
#define R_ARM_RELATIVE     23

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

/* ---- a view over the ELF image ---------------------------------------
 *
 * exec() decompresses the raw ELF into the task window, then slides every
 * PT_LOAD segment down to its link address.  After that, file offset N
 * lives wherever the segment containing it was moved - not at win+N.
 * This is the one place that translation happens: relocation, dynamic
 * table and entry-point code all read through view_at().  Program headers
 * are copied out before the slide so they survive having the ELF header
 * overwritten by the first segment. */
#define ELF_MAX_PHDR 16

typedef struct {
    uint8_t     *win;          /* the task window holding the image */
    uint32_t     size;         /* raw ELF byte count */
    uint32_t     win_size;     /* window byte count */
    Elf32_Ehdr   eh;
    Elf32_Phdr   ph[ELF_MAX_PHDR];
    uint32_t     phnum;
    uint32_t     link_lo;
} elf_view_t;

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

/* ---- what the program says about itself -------------------------------
 *
 * A program can ask for a particular stack or heap instead of taking what
 * the window would leave it: user/lib.h's TOS_STACK() and TOS_HEAP() drop
 * ELF notes in .note.tos, user.ld gives that section its own PT_NOTE
 * segment, and this walks it.  Both have to be read before the window is
 * sized - stack and heap are the two line items the window has to hold -
 * and they are requests, not facts, so process.c validates them against
 * what the port and the user area can describe.
 *
 * The name has to match: a toolchain is free to add .note.ABI-tag or
 * .note.gnu.property, and those must never be mistaken for a request. */
#define TOS_NOTE_STACK 1
#define TOS_NOTE_HEAP  2

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int elf_tos_note(const uint8_t *data, uint32_t size,
                 uint32_t *stack_out, uint32_t *heap_out)
{
    const Elf32_Ehdr *eh;
    int found = 0;

    if (stack_out)
        *stack_out = 0;
    if (heap_out)
        *heap_out = 0;
    if (header_ok(data, size, &eh) != 0)
        return -1;

    for (uint32_t i = 0; i < eh->e_phnum && found < 2; i++) {
        const Elf32_Phdr *ph = phdr_at(data, eh, i);
        uint32_t off, end;

        if (ph->p_type != PT_NOTE)
            continue;
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
            continue;
        off = ph->p_offset;
        end = off + ph->p_filesz;

        /* A note is three words of header, then the name and the
           descriptor, each padded to four bytes.  Keep walking after a
           hit: a program can carry both a stack and a heap request. */
        while (off <= end && end - off >= 12 && found < 2) {
            uint32_t namesz   = le32(data + off);
            uint32_t descsz   = le32(data + off + 4);
            uint32_t type     = le32(data + off + 8);
            uint32_t name_off = off + 12;
            uint32_t desc_off = name_off + ((namesz + 3u) & ~3u);
            uint32_t next     = desc_off + ((descsz + 3u) & ~3u);

            if (next < desc_off || next > end)
                break;      /* truncated, or a length that cannot be true */
            if (namesz >= 6 && descsz >= 4 &&
                memcmp(data + name_off, "TinyOS", 6) == 0) {
                if (type == TOS_NOTE_STACK && stack_out &&
                    *stack_out == 0) {
                    *stack_out = le32(data + desc_off);
                    found++;
                } else if (type == TOS_NOTE_HEAP && heap_out &&
                           *heap_out == 0) {
                    *heap_out = le32(data + desc_off);
                    found++;
                }
            }
            off = next;
        }
    }
    return 0;
}

/* ---- view construction ------------------------------------------------ */

static int view_read_header(const uint8_t *p, uint32_t size, elf_view_t *v)
{
    if (size < sizeof(Elf32_Ehdr))
        return -1;
    memcpy(&v->eh, p, sizeof(v->eh));
    if (v->eh.e_ident[0] != 0x7F || v->eh.e_ident[1] != 'E' ||
        v->eh.e_ident[2] != 'L' || v->eh.e_ident[3] != 'F')
        return -1;
    if (v->eh.e_ident[4] != ELFCLASS32)
        return -1;
    if (v->eh.e_phentsize != sizeof(Elf32_Phdr))
        return -1;
    if (v->eh.e_phnum == 0 || v->eh.e_phnum > ELF_MAX_PHDR)
        return -1;
    uint32_t ph_end = v->eh.e_phoff +
                      (uint32_t)v->eh.e_phnum * sizeof(Elf32_Phdr);
    if (ph_end > size)
        return -1;

    v->phnum = v->eh.e_phnum;
    memcpy(v->ph, p + v->eh.e_phoff, v->phnum * sizeof(Elf32_Phdr));
    v->link_lo = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < v->phnum; i++)
        if (v->ph[i].p_type == PT_LOAD && v->ph[i].p_vaddr < v->link_lo)
            v->link_lo = v->ph[i].p_vaddr;
    return v->link_lo == 0xFFFFFFFFu ? -1 : 0;
}

static int view_init(elf_view_t *v, uint8_t *win, uint32_t size,
                     uint32_t win_size)
{
    memset(v, 0, sizeof(*v));
    v->win = win;
    v->size = size;
    v->win_size = win_size;
    return view_read_header(win, size, v);
}

/* Where a run of raw file bytes can be read *now*.  Every PT_LOAD segment
   has been moved to `p_vaddr - link_lo`, so a file offset is translated
   through the segment that owns it.  Reading one entry at a time (which is
   what the callers do) keeps this independent of whether a whole table
   happens to fit inside a single segment. */
static int view_at(elf_view_t *v, uint32_t off, uint32_t len,
                   const uint8_t **out)
{
    if (off > v->size || len > v->size - off)
        return -1;
    for (uint32_t i = 0; i < v->phnum; i++) {
        const Elf32_Phdr *ph = &v->ph[i];
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
            continue;
        if (off < ph->p_offset || off + len > ph->p_offset + ph->p_filesz)
            continue;
        uint32_t dst = ph->p_vaddr - v->link_lo + (off - ph->p_offset);
        if (dst > v->win_size || len > v->win_size - dst)
            return -1;
        *out = v->win + dst;
        return 0;
    }
    return -1;
}

/* Convert a link-time virtual address inside the image to a file offset.
   Program headers give the vaddr -> file offset mapping for PT_LOAD
   segments; the dynamic table lives inside one of them. */
static int vaddr_to_off_view(elf_view_t *v, uint32_t va, uint32_t *off_out)
{
    for (uint32_t i = 0; i < v->phnum; i++) {
        const Elf32_Phdr *ph = &v->ph[i];
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
static int apply_relocations_view(elf_view_t *v, uint32_t bias)
{
    for (uint32_t i = 0; i < v->phnum; i++) {
        const Elf32_Phdr *ph = &v->ph[i];
        if (ph->p_type != PT_DYNAMIC)
            continue;
        if (ph->p_offset + ph->p_filesz > v->size)
            return -1;

        uint32_t ndyn = ph->p_filesz / sizeof(Elf32_Dyn);
        uint32_t rela = 0, relasz = 0, relaent = sizeof(Elf32_Rela);
        uint32_t rel = 0, relsz = 0, relent = 8;

        for (uint32_t d = 0; d < ndyn; d++) {
            const uint8_t *dp;
            if (view_at(v, ph->p_offset + d * sizeof(Elf32_Dyn),
                        sizeof(Elf32_Dyn), &dp) != 0)
                return -1;
            Elf32_Dyn de;
            memcpy(&de, dp, sizeof(de));
            if (de.d_tag == DT_NULL)
                break;
            switch (de.d_tag) {
            case DT_RELA:    rela = de.d_val; break;
            case DT_RELASZ:  relasz = de.d_val; break;
            case DT_RELAENT: relaent = de.d_val; break;
            case DT_REL:     rel = de.d_val; break;
            case DT_RELSZ:   relsz = de.d_val; break;
            default: break;
            }
        }
        if (relaent == 0)
            relaent = sizeof(Elf32_Rela);
        if (relent == 0)
            relent = 8;

        /* dt_val holds a *virtual address*; convert to a file offset. */
        uint32_t rela_off = 0, rel_off = 0;
        if (rela && vaddr_to_off_view(v, rela, &rela_off) != 0) {
            kprintf("elf: DT_RELA at 0x%x not mapped\n", rela);
            return -1;
        }
        if (rel && vaddr_to_off_view(v, rel, &rel_off) != 0) {
            kprintf("elf: DT_REL at 0x%x not mapped\n", rel);
            return -1;
        }

        /* .rela.dyn (explicit addend) */
        if (rela && relasz) {
            if (rela_off + relasz > v->size)
                return -1;
            for (uint32_t o = 0; o + sizeof(Elf32_Rela) <= relasz; o += relaent) {
                const uint8_t *rp;
                if (view_at(v, rela_off + o, sizeof(Elf32_Rela), &rp) != 0)
                    return -1;
                Elf32_Rela r;
                memcpy(&r, rp, sizeof(r));

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
                /* ARM links emit .rel.dyn, never .rela.dyn, so these are
                   belt-and-braces for the explicit-addend path. */
                case R_ARM_RELATIVE:
                case R_ARM_ABS32:
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT:
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
            if (rel_off + relsz > v->size) {
                kprintf("elf: .rel past end (off=0x%x sz=0x%x size=0x%x)\n",
                        rel_off, relsz, v->size);
                return -1;
            }
            for (uint32_t o = 0; o + 8 <= relsz; o += relent) {
                const uint8_t *rp;
                if (view_at(v, rel_off + o, 8, &rp) != 0)
                    return -1;
                uint32_t r_offset, r_info;
                memcpy(&r_offset, rp, 4);
                memcpy(&r_info, rp + 4, 4);

                uint32_t type = R_TYPE(r_info);
                if (type == R_386_NONE)
                    continue;      /* same numeric value as R_ARM_NONE */

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
                /* ARM (Cortex-M): .rel.dyn with the addend stored in place.
                   See the R_ARM_* comment above - all reduce to val += bias. */
                case R_ARM_RELATIVE:
                case R_ARM_ABS32:
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT:
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

static const char *machine_name(uint16_t m)
{
    switch (m) {
    case EM_386:   return "x86";
    case EM_ARM:   return "arm";
    case EM_RISCV: return "rv32";
    default:       return "unknown";
    }
}

/* Shared tail: check machine/type, work out the bias, clear .bss (the
   segments have already been placed in the window), apply relocations and
   hand back the entry point. */
static int load_view(elf_view_t *v, uint32_t load_base, uint32_t *entry_out)
{
#if defined(__riscv)
    const uint16_t expected_machine = EM_RISCV;
#elif defined(__i386__)
    const uint16_t expected_machine = EM_386;
#elif defined(__arm__)
    const uint16_t expected_machine = EM_ARM;
#else
    const uint16_t expected_machine = v->eh.e_machine;
#endif
    if (v->eh.e_machine != expected_machine) {
        /* Worth a message: the symptom is a working kernel that refuses every
           program on a filesystem built for another architecture, and
           "not a valid ELF32 program" sends you looking at the image. */
        kprintf("elf: program is machine %u (%s), this kernel runs machine %u (%s)\n",
                v->eh.e_machine, machine_name(v->eh.e_machine),
                expected_machine, machine_name(expected_machine));
        return -1;
    }

    if (v->eh.e_type != ET_EXEC && v->eh.e_type != ET_DYN)
        return -1;

    /* ET_EXEC: the image must run where it was linked -> bias 0.
       ET_DYN : the image is linked at 0, so a link-time address A must be
                placed at load_base + A.  bias = load_base - link_lo. */
    uint32_t bias = 0;
    if (v->eh.e_type == ET_DYN)
        bias = load_base - v->link_lo;

    for (uint32_t i = 0; i < v->phnum; i++) {
        const Elf32_Phdr *ph = &v->ph[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_offset + ph->p_filesz > v->size) {
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
        /* the segment is already at dst; only .bss remains */
        if (ph->p_memsz > ph->p_filesz) {
            if (zero_user(dst + ph->p_filesz, ph->p_memsz - ph->p_filesz) != 0)
                return -1;
        }
    }

    if (apply_relocations_view(v, bias) != 0)
        return -1;

    uint32_t entry = v->eh.e_entry + bias;
    if (!user_range_ok(entry, 1)) {
        kprintf("elf: entry 0x%x outside app window\n", entry);
        return -1;
    }
    *entry_out = entry;
    return 0;
}

/* Load an image that was decompressed straight into the task window.
 *
 * `win` holds the raw ELF as one linear byte stream (a LZ4 match may look
 * back 64 KiB into already-produced output, so the decompressor cannot skip
 * the ELF header or scatter segments as it goes).  This function copies the
 * headers out, slides every PT_LOAD to `p_vaddr - link_lo`, then clears
 * .bss, applies relocations and returns the entry point.  All 36 staged
 * programs have been checked to have forward-only, non-overlapping slides. */
int elf_load(uint8_t *win, uint32_t size, uint32_t win_size,
             uint32_t load_base, uint32_t *entry_out)
{
    elf_view_t v;
    if (view_init(&v, win, size, win_size) != 0)
        return -1;

    uint32_t prev_dst_end = 0;
    for (uint32_t i = 0; i < v.phnum; i++) {
        const Elf32_Phdr *ph = &v.ph[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_offset + ph->p_filesz > v.size) {
            kprintf("elf: segment %u out of file\n", i);
            return -1;
        }
        uint32_t dst = ph->p_vaddr - v.link_lo;
        uint32_t src = ph->p_offset;
        /* A destination below its source means a forward memmove cannot
           clobber the source of a later segment.  Anything else needs a
           different traversal order; refuse it rather than corrupt data. */
        if (dst > src || dst < prev_dst_end ||
            dst > v.win_size || ph->p_memsz > v.win_size - dst) {
            kprintf("elf: segment %u cannot be placed in the window\n", i);
            return -1;
        }
        memmove(v.win + dst, v.win + src, ph->p_filesz);
        prev_dst_end = dst + ph->p_filesz;
    }
    return load_view(&v, load_base, entry_out);
}
