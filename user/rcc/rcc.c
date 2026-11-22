/* rcc: RIR2 -> RV32 ELF (TinyOS userland), C implementation.
 *
 * This is the board-side counterpart of rvm.  It emits machine code
 * directly, writes an ELF32 ET_DYN image and wraps it in a stored LZ4TOS
 * envelope so exec() accepts it.  No assembler text and no libc are needed.
 */
#include "lib.h"
#include <string.h>
#include <stdint.h>

TOS_STACK(16 * 1024);

#if defined(__riscv) || defined(__arm__)
#define RIR_MAX   40960
#define TEXT_MAX  49152
#define LABEL_MAX 4096
#define PC_MAX    2048
#define FUNC_MAX  256
#else
#define RIR_MAX   262144
#define TEXT_MAX  262144
#define LABEL_MAX 16384
#define PC_MAX    16384
#define FUNC_MAX  1024
#endif
#define PC_LABEL_NONE 0xFFFFu
#define TEXT_OFF  0x100

enum {
    R_CONST, R_MOV, R_ADD, R_SUB, R_MUL, R_DIV,
    R_LT, R_GT, R_EQ, R_JZ, R_JMP, R_RET, R_HALT,
    R_LOADG, R_STOREG, R_LOADA, R_STOREA, R_PRINT,
    R_ARG, R_CALL, R_LOADL, R_STOREL,
    R_INC, R_DEC, R_ADDI, R_SUBI, R_MULI,
    R_JLT, R_JGE, R_JGT, R_JLE, R_JEQ, R_JNE,
    R_JLT_I, R_JGE_I, R_JGT_I, R_JLE_I, R_JEQ_I, R_JNE_I,
    R_STOREA_I, R_NOP, R_PRINTS, R_PRINTN, R_PRINTV,
    R_NATIVE, R_NATIVER, R_LOADD, R_STORED,
    R_SHLI, R_SHRI, R_FMUL, R_FDIV, R_PRINTFIX, R_PRINTFIXV,
    R_BOUNDS, R_PRINTFIXN, R_MOD,
    R_LOADB, R_STOREB, R_STRCPY, R_STRCAT,
    R_STRLEN, R_STRCMP, R_ATOI, R_PRINTSTR,
    R_NE, R_LE, R_GE, R_NATIVEC
};

/* The RIR2 image is read in 4 KiB blocks.  Both passes scan the instruction
 * array sequentially, so a sliding window is all a compiler this size needs:
 * a 40 KiB image costs about ten reads per pass.  The function table, string
 * pool and arena are small and are kept separately. */
#define RIR_BLK 4096
static uint8_t rir_blk[RIR_BLK];
static int rir_fd;
static int rir_len;
static int blk_off = -1;
static int blk_len;
static int code_off;
static uint8_t func_tab[FUNC_MAX * 4];
static uint8_t str_buf[1024];
static uint8_t arena_buf[512];

static int text_len;
static int out_len;
static const char *out_path;
static int emit_pass;
static int out_fd;

static int nir;
static int gsize;
static int entry_pc;
static int nfunc;
static const uint8_t *strpool;
static int strsize;
static const uint8_t *arena;
static int dsize;

static int label_off[LABEL_MAX];
static uint8_t label_seen[LABEL_MAX];
static int label_next;
static int label_total;
static int label_base;
static uint16_t pc_label[PC_MAX];
static int func_label[FUNC_MAX];

static void die(const char *s)
{
    puts("rcc: ");
    puts(s);
    puts("\n");
    exit(1);
}

static int write_all_fd(int fd, const uint8_t *p, int n);

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void rir_read(void *dst, int off, int len)
{
    uint8_t *p = dst;
    if (off < 0 || len < 0 || off > rir_len - len)
        die("short RIR2 image");
    while (len > 0) {
        int avail, k;
        if (off < blk_off || off >= blk_off + blk_len) {
            blk_off = off & ~(RIR_BLK - 1);
            if (lseek(rir_fd, blk_off, 0) < 0)
                die("seek failed");
            blk_len = read(rir_fd, rir_blk, RIR_BLK);
            if (blk_len <= 0)
                die("read failed");
        }
        avail = blk_off + blk_len - off;
        k = len < avail ? len : avail;
        memcpy(p, rir_blk + (off - blk_off), (size_t)k);
        p += k;
        off += k;
        len -= k;
    }
}

static uint32_t rir_u32(int off)
{
    uint8_t b[4];
    rir_read(b, off, 4);
    return get32(b);
}

static int32_t ir_a(int pc) { return (int32_t)rir_u32(code_off + pc * 20 + 4); }
static int32_t ir_b(int pc) { return (int32_t)rir_u32(code_off + pc * 20 + 8); }
static int32_t ir_c(int pc) { return (int32_t)rir_u32(code_off + pc * 20 + 12); }
static int32_t ir_d(int pc) { return (int32_t)rir_u32(code_off + pc * 20 + 16); }
static int ir_op(int pc) { return (int32_t)rir_u32(code_off + pc * 20); }

static void e32(uint32_t v)
{
    uint8_t b[4];
    if (text_len + 4 > TEXT_MAX)
        die("text overflow");
    if (emit_pass == 2) {
        put32(b, v);
        if (write_all_fd(out_fd, b, 4) < 0)
            die("write failed");
    }
    text_len += 4;
}

static void e_bytes(const uint8_t *p, int n)
{
    if (text_len + n > TEXT_MAX)
        die("text overflow");
    if (emit_pass == 2 && write_all_fd(out_fd, p, n) < 0)
        die("write failed");
    text_len += n;
}

/* ---- labels and fixups ---------------------------------------------- */

static int alloc_label(void)
{
    int l = label_next++;
    if (l >= LABEL_MAX)
        die("too many labels");
    if (emit_pass == 2 && l >= label_total)
        die("label order mismatch");
    if (l >= label_total) {
        label_off[l] = -1;
        label_total = l + 1;
    }
    return l;
}

static int new_label(void)
{
    return alloc_label();
}

static void bind_label(int l)
{
    label_off[l] = text_len;
}

static int data_strpool, data_arena, data_g, data_argbuf, data_pwmreq;
static int data_statbuf;
static int data_va;

/* ---- RIR program analysis ------------------------------------------- */

static int func_start[FUNC_MAX];
static int func_end[FUNC_MAX];
static int func_order[FUNC_MAX];
static int max_argc[FUNC_MAX];
static int main_fid = -1;

static void analyze(void)
{
    int i, j;
    if (nir <= 0 || nfunc <= 0 || nfunc > FUNC_MAX)
        die("bad image header");
    if (entry_pc < 0 || entry_pc >= nir)
        die("bad entry pc");
    for (i = 0; i < nfunc; i++)
        func_start[i] = (int)get32(func_tab + i * 4);
    for (i = 0; i < nfunc; i++) {
        func_end[i] = nir;
        for (j = 0; j < nfunc; j++)
            if (func_start[j] > func_start[i] && func_start[j] < func_end[i])
                func_end[i] = func_start[j];
        if (func_start[i] < 0 || func_start[i] >= func_end[i])
            die("bad function range");
        if (func_start[i] == entry_pc)
            main_fid = i;
        max_argc[i] = 0;
    }
    if (main_fid < 0)
        die("entry is not a function start");
    for (i = 0; i < nfunc; i++)
        func_order[i] = i;
    for (i = 1; i < nfunc; i++) {
        int x = func_order[i];
        j = i - 1;
        while (j >= 0 && func_start[func_order[j]] > func_start[x]) {
            func_order[j + 1] = func_order[j];
            j--;
        }
        func_order[j + 1] = x;
    }
    for (i = 0; i < nir; i++) {
        if (ir_op(i) == R_CALL) {
            int fid = ir_a(i), argc = ir_c(i);
            if (fid < 0 || fid >= nfunc)
                die("bad CALL target");
            if (argc > max_argc[fid])
                max_argc[fid] = argc;
        }
    }
    for (i = 0; i < PC_MAX; i++)
        pc_label[i] = PC_LABEL_NONE;
    for (i = 0; i < FUNC_MAX; i++)
        func_label[i] = -1;
}

/* RIR registers above VREG_IN_REG spill to the frame; R_LOADL/R_STOREL slots
 * start right after them.  Keep this in step with the backend's LOCAL_BASE. */
#if defined(RCC_ARCH_THUMB1)
#define RIR_SLOT_BYTES 48      /* 12 slots: RIR 4..15 */
#else
#define RIR_SLOT_BYTES 36      /*  9 slots: RIR 7..15 */
#endif

static int frame_for(int fid)
{
    int pc, maxslot = -1, frame;
    for (pc = func_start[fid]; pc < func_end[fid]; pc++) {
        int op = ir_op(pc);
        if (op == R_LOADL && ir_a(pc) > maxslot)
            maxslot = ir_a(pc);
        if (op == R_STOREL && ir_b(pc) > maxslot)
            maxslot = ir_b(pc);
    }
    frame = 20 + RIR_SLOT_BYTES + 4 * (maxslot + 1);
    if (frame < 64)
        frame = 64;
    return (frame + 15) & ~15;
}

#if defined(__riscv) || defined(RCC_ARCH_RV32)
#include "rv32.inc"
#elif defined(RCC_ARCH_THUMB1)
#include "thumb1.inc"
#elif defined(__arm__) || defined(RCC_ARCH_THUMB2)
#include "thumb2.inc"
#else
#error "rcc: no backend selected (RCC_ARCH_RV32 / RCC_ARCH_THUMB1 / RCC_ARCH_THUMB2)"
#endif

static void write_elf_header(int data_off, int data_filesz, int data_memsz)
{
    uint8_t h[TEXT_OFF];
    memset(h, 0, sizeof h);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
    h[4] = 1; h[5] = 1; h[6] = 1;
    put16(h + 16, 3);                      /* ET_DYN */
    put16(h + 18, (uint16_t)rcc_arch_machine());                    /* EM_RISCV */
    put32(h + 20, 1);
    put32(h + 24, 0);                      /* entry */
    put32(h + 28, 52);                     /* phoff */
    put32(h + 32, 0);                      /* shoff */
    put32(h + 36, rcc_arch_flags());
    put16(h + 40, 52);
    put16(h + 42, 32);
    put16(h + 44, 2);
    put16(h + 46, 0);
    put16(h + 48, 0);
    put16(h + 50, 0);

    /* PT_LOAD text */
    put32(h + 52, 1);
    put32(h + 56, TEXT_OFF);
    put32(h + 60, 0);
    put32(h + 64, 0);
    put32(h + 68, (uint32_t)text_len);
    put32(h + 72, (uint32_t)text_len);
    put32(h + 76, 5);
    put32(h + 80, 16);

    /* PT_LOAD data+bss */
    put32(h + 84, 1);
    put32(h + 88, (uint32_t)data_off);
    put32(h + 92, (uint32_t)data_va);
    put32(h + 96, (uint32_t)data_va);
    put32(h + 100, (uint32_t)data_filesz);
    put32(h + 104, (uint32_t)data_memsz);
    put32(h + 108, 6);
    put32(h + 112, 16);

    if (write_all_fd(out_fd, h, TEXT_OFF) < 0)
        die("write failed");
}

static int lz4_size(int n)
{
    if (n < 15)
        return 1 + n;
    return 1 + 1 + (n - 15) / 255 + n;
}

static int write_all_fd(int fd, const uint8_t *p, int n)
{
    int off = 0;
    while (off < n) {
        int w = write(fd, p + off, n - off);
        if (w <= 0)
            return -1;
        off += w;
    }
    return 0;
}

static int lz4_token(int fd, int n)
{
    /* Stream the extended length one byte at a time.  A fixed 256-byte
     * buffer overflowed once the output passed ~65 KB (issue #6): the token
     * itself is 2 + (n-15)/255 bytes long. */
    uint8_t b;
    int rem;

    if (n < 15) {
        b = (uint8_t)(n << 4);
        return write_all_fd(fd, &b, 1);
    }
    b = 0xf0;
    if (write_all_fd(fd, &b, 1) < 0)
        return -1;
    rem = n - 15;
    b = 255;
    while (rem >= 255) {
        if (write_all_fd(fd, &b, 1) < 0)
            return -1;
        rem -= 255;
    }
    b = (uint8_t)rem;
    return write_all_fd(fd, &b, 1);
}

static int load_rir(const char *path)
{
    rir_fd = open(path);
    if (rir_fd < 0)
        return -1;
    rir_len = lseek(rir_fd, 0, 2);      /* SEEK_END */
    if (rir_len < 28)
        return -1;
    blk_off = -1;
    blk_len = 0;
    return 0;
}

static void parse_rir(void)
{
    uint8_t h[28];
    uint32_t magic;
    int func_off, str_off, arena_off;
    if (rir_len < 28)
        die("short RIR2 header");
    if (rir_len > RIR_MAX)
        die("RIR2 image too large");   /* same accepted input as the old buffer */
    rir_read(h, 0, 28);
    magic = get32(h);
    if (magic != 0x32524952u)
        die("not a RIR2 image");
    nir = (int)get32(h + 4);
    gsize = (int)get32(h + 8);
    entry_pc = (int)get32(h + 12);
    nfunc = (int)get32(h + 16);
    strsize = (int)get32(h + 20);
    dsize = (int)get32(h + 24);
    if (nir <= 0 || gsize < 0 || nfunc <= 0 || nfunc > FUNC_MAX ||
        strsize < 0 || strsize > (int)sizeof str_buf ||
        dsize < 0 || dsize > (int)sizeof arena_buf)
        die("bad RIR2 header");
    code_off = 28;
    func_off = code_off + nir * 20;
    str_off = func_off + nfunc * 4;
    arena_off = str_off + strsize;
    if ((int64_t)arena_off + dsize > (int64_t)rir_len)
        die("truncated RIR2 image");
    rir_read(func_tab, func_off, nfunc * 4);
    if (strsize > 0)
        rir_read(str_buf, str_off, strsize);
    if (dsize > 0)
        rir_read(arena_buf, arena_off, dsize);
    strpool = str_buf;
    arena = arena_buf;
}

static int align16(int v) { return (v + 15) & ~15; }
static int align4k(int v) { return (v + 4095) & ~4095; }

static void write_zeros(int fd, int n)
{
    static const uint8_t z[64];
    while (n > 0) {
        int k = n > (int)sizeof z ? (int)sizeof z : n;
        if (write_all_fd(fd, z, k) < 0)
            die("write failed");
        n -= k;
    }
}

static void generate(void)
{
    int data_off, data_filesz, bss, g_bytes, arena_bytes, argbuf_off, pwm_off;
    int text_final, span, blen, fd;

    data_strpool = new_label();
    data_arena = new_label();
    data_g = new_label();
    data_argbuf = new_label();
    data_pwmreq = new_label();
    data_statbuf = new_label();
    analyze();
    rcc_arch_init_labels();
    label_base = label_next;

    emit_pass = 1;
    rcc_arch_emit_body();
    text_final = text_len;

    {
        int da = rcc_arch_data_align();
        data_va = (text_final + da - 1) & ~(da - 1);
        data_off = (TEXT_OFF + text_final + da - 1) & ~(da - 1);
    }
    arena_bytes = (dsize + 3) & ~3;
    data_filesz = arena_bytes;
    g_bytes = gsize * 4;
    if (g_bytes <= 0)
        g_bytes = 4;
    argbuf_off = (g_bytes + 3) & ~3;
    pwm_off = argbuf_off + 64;
    bss = pwm_off + 12 + 8;

    label_off[data_arena] = data_va;
    label_off[data_g] = data_va + arena_bytes;
    label_off[data_argbuf] = data_va + arena_bytes + argbuf_off;
    label_off[data_pwmreq] = data_va + arena_bytes + pwm_off;
    label_off[data_statbuf] = data_va + arena_bytes + pwm_off + 12;
    data_bss_size = bss;

    out_len = data_off + data_filesz;
    /* The LZ4 header carries a 32-bit length, but generating a multi-GB
     * image is a bug rather than a useful outcome. */
    if (out_len > 64 * 1024 * 1024)
        die("output too large (over 64 MiB)");
    span = align4k(data_va + data_filesz + bss);
    blen = lz4_size(out_len);

    fd = create(out_path);
    if (fd < 0)
        die("cannot create output");
    out_fd = fd;
    {
        uint8_t hdr[28];
        memset(hdr, 0, sizeof hdr);
        memcpy(hdr, "LZ4TOS", 6);
        put16(hdr + 6, 1);
        put32(hdr + 8, (uint32_t)out_len);
        put32(hdr + 12, (uint32_t)blen);
        put32(hdr + 16, (uint32_t)span);
        put32(hdr + 20, 0);
        put32(hdr + 24, 0);
        if (write_all_fd(fd, hdr, 28) < 0)
            die("write failed");
    }
    if (lz4_token(fd, out_len) < 0)
        die("write failed");
    text_len = text_final;
    write_elf_header(data_off, data_filesz, data_filesz + bss);

    emit_pass = 2;
    label_next = label_base;
    memset(label_seen, 0, sizeof label_seen);
    rcc_arch_emit_body();
    if (text_len != text_final)
        die("pass size mismatch");
    write_zeros(fd, data_off - (TEXT_OFF + text_final));
    if (dsize > 0 && write_all_fd(fd, arena, dsize) < 0)
        die("write failed");
    write_zeros(fd, arena_bytes - dsize);
    close(fd);

    puts("rcc: ");
    putint(text_final);
    puts(" text, ");
    putint(out_len);
    puts(" elf, ");
    putint(blen + 28);
    puts(" tos, ");
    putint(rcc_arch_labels());
    puts(" labels\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        puts("usage: rcc input.rir output.tos\n");
        return 2;
    }
    out_path = argv[2];
    if (load_rir(argv[1]) < 0)
        die("cannot read input");
    parse_rir();
    generate();
    return 0;
}
