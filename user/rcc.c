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

#ifdef __riscv
#define RIR_MAX   16384
#define TEXT_MAX  24576
#define LABEL_MAX 768
#else
#define RIR_MAX   65536
#define TEXT_MAX  65536
#define LABEL_MAX 2048
#endif
#define PC_MAX    1024
#define FUNC_MAX  32
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
    R_NE, R_LE, R_GE
};

static uint8_t rir_buf[RIR_MAX];

static int rir_len;
static int text_len;
static int out_len;
static const char *out_path;
static int emit_pass;
static int out_fd;

static const uint8_t *code;
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
static int pc_label[PC_MAX];
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

static int32_t ir_a(int pc) { return (int32_t)get32(code + pc * 20 + 4); }
static int32_t ir_b(int pc) { return (int32_t)get32(code + pc * 20 + 8); }
static int32_t ir_c(int pc) { return (int32_t)get32(code + pc * 20 + 12); }
static int32_t ir_d(int pc) { return (int32_t)get32(code + pc * 20 + 16); }
static int ir_op(int pc) { return (int32_t)get32(code + pc * 20); }

/* ---- register mapping and instruction encoders ---------------------- */

enum {
    X_ZERO = 0, X_RA = 1, X_SP = 2, X_GP = 3,
    X_T0 = 5, X_T1 = 6, X_T2 = 7,
    X_S0 = 8, X_S1 = 9,
    X_A0 = 10, X_A1, X_A2, X_A3, X_A4, X_A5, X_A6, X_A7,
    X_S2 = 18, X_S3, X_S4, X_S5, X_S6, X_S7, X_S8, X_S9, X_S10, X_S11,
    X_T3 = 28, X_T4, X_T5, X_T6
};

static int rreg(int n)
{
    if (n >= 0 && n <= 1)
        return X_S0 + n;
    if (n >= 2 && n <= 11)
        return X_S2 + (n - 2);
    if (n == 12)
        return X_GP;
    if (n >= 13 && n <= 15)
        return X_T0 + (n - 13);
    die("bad RIR register");
    return 0;
}

static uint32_t enc_r(uint32_t f7, int rd, int rs1, int rs2, uint32_t f3)
{
    return (f7 << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) |
           (f3 << 12) | ((uint32_t)rd << 7) | 0x33;
}

static uint32_t enc_i(int32_t imm, int rd, int rs1, uint32_t f3, uint32_t op)
{
    return (((uint32_t)imm & 0xfff) << 20) | ((uint32_t)rs1 << 15) |
           (f3 << 12) | ((uint32_t)rd << 7) | op;
}

static uint32_t enc_s(int32_t imm, int rs2, int rs1, uint32_t f3)
{
    uint32_t u = (uint32_t)imm;
    return ((u >> 5 & 0x7f) << 25) | ((uint32_t)rs2 << 20) |
           ((uint32_t)rs1 << 15) | (f3 << 12) | ((u & 0x1f) << 7) | 0x23;
}

static uint32_t enc_b(int32_t off, int rs1, int rs2, uint32_t f3)
{
    uint32_t u = (uint32_t)off;
    return ((u >> 12 & 1) << 31) | ((u >> 5 & 0x3f) << 25) |
           ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | (f3 << 12) |
           ((u >> 1 & 0xf) << 8) | ((u >> 11 & 1) << 7) | 0x63;
}

static uint32_t enc_j(int32_t off, int rd)
{
    uint32_t u = (uint32_t)off;
    return ((u >> 20 & 1) << 31) | ((u >> 1 & 0x3ff) << 21) |
           ((u >> 11 & 1) << 20) | ((u >> 12 & 0xff) << 12) |
           ((uint32_t)rd << 7) | 0x6f;
}

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

static void e_add(int rd, int rs1, int rs2) { e32(enc_r(0, rd, rs1, rs2, 0)); }
static void e_sub(int rd, int rs1, int rs2) { e32(enc_r(0x20, rd, rs1, rs2, 0)); }
static void e_mul(int rd, int rs1, int rs2) { e32(enc_r(1, rd, rs1, rs2, 0)); }
static void e_div(int rd, int rs1, int rs2) { e32(enc_r(1, rd, rs1, rs2, 4)); }
static void e_divu(int rd, int rs1, int rs2) { e32(enc_r(1, rd, rs1, rs2, 5)); }
static void e_rem(int rd, int rs1, int rs2) { e32(enc_r(1, rd, rs1, rs2, 6)); }
static void e_remu(int rd, int rs1, int rs2) { e32(enc_r(1, rd, rs1, rs2, 7)); }
static void e_slt(int rd, int rs1, int rs2) { e32(enc_r(0, rd, rs1, rs2, 2)); }
static void e_sltu(int rd, int rs1, int rs2) { e32(enc_r(0, rd, rs1, rs2, 3)); }
static void e_sltiu(int rd, int rs1, int32_t imm) { e32(enc_i(imm, rd, rs1, 3, 0x13)); }
static void e_sll(int rd, int rs1, int rs2) { e32(enc_r(0, rd, rs1, rs2, 1)); }
static void e_xor(int rd, int rs1, int rs2) { e32(enc_r(0, rd, rs1, rs2, 4)); }
static void e_or(int rd, int rs1, int rs2) { e32(enc_r(0, rd, rs1, rs2, 6)); }
static void e_and(int rd, int rs1, int rs2) { e32(enc_r(0, rd, rs1, rs2, 7)); }
static void e_addi(int rd, int rs1, int32_t imm) { e32(enc_i(imm, rd, rs1, 0, 0x13)); }
static void e_andi(int rd, int rs1, int32_t imm) { e32(enc_i(imm, rd, rs1, 7, 0x13)); }
static void e_xori(int rd, int rs1, int32_t imm) { e32(enc_i(imm, rd, rs1, 4, 0x13)); }
static void e_slli(int rd, int rs1, int sh) { e32(enc_i(sh & 31, rd, rs1, 1, 0x13)); }
static void e_srli(int rd, int rs1, int sh) { e32(enc_i(sh & 31, rd, rs1, 5, 0x13)); }
static void e_srai(int rd, int rs1, int sh) { e32(enc_i((sh & 31) | 0x400, rd, rs1, 5, 0x13)); }
static void e_lw(int rd, int32_t off, int rs1) { e32(enc_i(off, rd, rs1, 2, 0x03)); }
static void e_lbu(int rd, int32_t off, int rs1) { e32(enc_i(off, rd, rs1, 4, 0x03)); }
static void e_sw(int rs2, int32_t off, int rs1) { e32(enc_s(off, rs2, rs1, 2)); }
static void e_sb(int rs2, int32_t off, int rs1) { e32(enc_s(off, rs2, rs1, 0)); }
static void e_ecall(void) { e32(0x00000073); }
static void e_nop(void) { e_addi(X_ZERO, X_ZERO, 0); }
static void e_mv(int rd, int rs) { e_addi(rd, rs, 0); }
static void e_bne(int rs1, int rs2, int32_t off) { e32(enc_b(off, rs1, rs2, 1)); }
static void e_beq(int rs1, int rs2, int32_t off) { e32(enc_b(off, rs1, rs2, 0)); }
static void e_blt(int rs1, int rs2, int32_t off) { e32(enc_b(off, rs1, rs2, 4)); }
static void e_bge(int rs1, int rs2, int32_t off) { e32(enc_b(off, rs1, rs2, 5)); }
static void e_bltu(int rs1, int rs2, int32_t off) { e32(enc_b(off, rs1, rs2, 6)); }
static void e_bgeu(int rs1, int rs2, int32_t off) { e32(enc_b(off, rs1, rs2, 7)); }

static void e_li(int rd, int32_t v)
{
    if (v >= -2048 && v <= 2047) {
        e_addi(rd, X_ZERO, v);
        return;
    }
    {
        int32_t hi = (v + 0x800) >> 12;
        int32_t lo = v - (hi << 12);
        e32(((uint32_t)hi & 0xfffff) << 12 | ((uint32_t)rd << 7) | 0x37);
        if (lo)
            e_addi(rd, rd, lo);
    }
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

static int pc_label_for(int pc)
{
    int l;
    if (pc < 0 || pc >= PC_MAX)
        die("pc out of range");
    if (pc_label[pc] >= 0) {
        if (emit_pass == 2 && !label_seen[pc_label[pc]]) {
            l = alloc_label();
            label_seen[pc_label[pc]] = 1;
            if (l != pc_label[pc])
                die("label order mismatch");
        }
        return pc_label[pc];
    }
    l = alloc_label();
    pc_label[pc] = l;
    return l;
}

static int data_strpool, data_arena, data_g, data_argbuf, data_pwmreq;
static int data_va;

static void e_la(int rd, int label)
{
    if (emit_pass == 1) {
        text_len += 8;
        return;
    }
    {
        int32_t d = label_off[label] - text_len;
        int32_t hi = (d + 0x800) >> 12;
        int32_t lo = d - (hi << 12);
        e32(((uint32_t)hi & 0xfffff) << 12 | ((uint32_t)rd << 7) | 0x17);
        e_addi(rd, rd, lo);
    }
}

static void e_call(int label)
{
    if (emit_pass == 1) {
        text_len += 8;
        return;
    }
    {
        int32_t d = label_off[label] - text_len;
        int32_t hi = (d + 0x800) >> 12;
        int32_t lo = d - (hi << 12);
        e32(((uint32_t)hi & 0xfffff) << 12 | ((uint32_t)X_RA << 7) | 0x17);
        e32(enc_i(lo, X_RA, X_RA, 0, 0x67));
    }
}

static void e_j(int label)
{
    if (emit_pass == 1) {
        text_len += 4;
        return;
    }
    e32(enc_j(label_off[label] - text_len, X_ZERO));
}

static void e_branch_to(int f3, int rs1, int rs2, int label)
{
    if (emit_pass == 1) {
        text_len += 4;
        return;
    }
    e32(enc_b(label_off[label] - text_len, rs1, rs2, (uint32_t)f3));
}

/* Branch when the condition is true, using an inverted near branch over a
 * JAL, so only the ±1 MiB JAL range matters. */
static void cond_jump(int inv_f3, int rs1, int rs2, int target)
{
    int skip = new_label();
    e_branch_to(inv_f3, rs1, rs2, skip);
    e_j(target);
    bind_label(skip);
}

static void cond_jump_zero(int rs, int target)
{
    int skip = new_label();
    e_branch_to(1, rs, X_ZERO, skip);     /* bne rs, zero, skip */
    e_j(target);
    bind_label(skip);
}

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
        func_start[i] = (int)get32(rir_buf + 28 + nir * 20 + i * 4);
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
        pc_label[i] = -1;
    for (i = 0; i < FUNC_MAX; i++)
        func_label[i] = -1;
}

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
    frame = 56 + 4 * (maxslot + 1);
    if (frame < 64)
        frame = 64;
    return (frame + 15) & ~15;
}

/* ---- emission -------------------------------------------------------- */

static int L_bounds, L_pi, L_pin, L_picommon, L_par;
static int L_scmp, L_slen, L_scpy, L_scat, L_atoi;
static int L_fmul, L_fdiv, L_fix, L_fixn0, L_fixn;
static int rodata_msg, rodata_p10, rodata_empty;
static int data_bss_size;

static int func_label_for(int fid)
{
    int l;
    if (func_label[fid] >= 0) {
        if (emit_pass == 2 && !label_seen[func_label[fid]]) {
            l = alloc_label();
            label_seen[func_label[fid]] = 1;
            if (l != func_label[fid])
                die("label order mismatch");
        }
        return func_label[fid];
    }
    l = alloc_label();
    func_label[fid] = l;
    return l;
}

static int func_epi[FUNC_MAX];

static void emit_prologue(int fid, int frame, int argc)
{
    int i;
    bind_label(func_label_for(fid));
    e_addi(X_SP, X_SP, -frame);
    e_sw(X_RA, 0, X_SP);
    for (i = 0; i < 12; i++)
        e_sw(rreg(i), 4 + 4 * i, X_SP);
    e_sw(X_GP, 52, X_SP);
    if (argc > 0) {
        int n = argc > 13 ? 13 : argc;
        e_la(X_T3, data_argbuf);
        for (i = 0; i < n; i++)
            e_lw(rreg(i), 4 * i, X_T3);
    }
}

static void emit_epilogue(int fid, int frame)
{
    int i;
    bind_label(func_epi[fid]);
    e_lw(X_RA, 0, X_SP);
    for (i = 0; i < 12; i++)
        e_lw(rreg(i), 4 + 4 * i, X_SP);
    e_lw(X_GP, 52, X_SP);
    e_addi(X_SP, X_SP, frame);
    e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));      /* ret */
}

static void emit_native(int op, int id, int a0, int a1, int a2, int dst)
{
    int d = (op == R_NATIVER) ? dst : -1;
    int r0 = a0 < 0 ? X_ZERO : rreg(a0);
    int r1 = a1 < 0 ? X_ZERO : rreg(a1);
    int r2 = a2 < 0 ? X_ZERO : rreg(a2);
    switch (id) {
    case 0: /* gpio_count */
        e_li(X_A0, 0); e_li(X_A1, 0); e_li(X_A2, 0); e_li(X_A7, 26); e_ecall();
        if (d >= 0) e_mv(rreg(d), X_A0);
        break;
    case 1: /* gpio_mode */
        e_li(X_A0, 1); e_mv(X_A1, r0); e_mv(X_A2, r1); e_li(X_A7, 26); e_ecall();
        break;
    case 2: /* gpio_write */
        e_li(X_A0, 2); e_mv(X_A1, r0); e_mv(X_A2, r1); e_li(X_A7, 26); e_ecall();
        break;
    case 3: /* gpio_read */
        e_li(X_A0, 3); e_mv(X_A1, r0); e_li(X_A2, 0); e_li(X_A7, 26); e_ecall();
        if (d >= 0) e_mv(rreg(d), X_A0);
        break;
    case 4: /* adc_count */
        e_li(X_A0, 0); e_li(X_A1, 0); e_li(X_A7, 27); e_ecall();
        if (d >= 0) e_mv(rreg(d), X_A0);
        break;
    case 5: /* adc_read */
        e_li(X_A0, 1); e_mv(X_A1, r0); e_li(X_A7, 27); e_ecall();
        if (d >= 0) e_mv(rreg(d), X_A0);
        break;
    case 6: /* pwm_count */
        e_li(X_A0, 0); e_li(X_A1, 0); e_li(X_A7, 40); e_ecall();
        if (d >= 0) e_mv(rreg(d), X_A0);
        break;
    case 7: /* pwm_set */
        e_la(X_T3, data_pwmreq);
        e_sw(r0, 0, X_T3);
        e_sw(r1, 4, X_T3);
        e_sw(r2, 8, X_T3);
        e_li(X_A0, 1); e_la(X_A1, data_pwmreq); e_li(X_A2, 0); e_li(X_A7, 40); e_ecall();
        break;
    case 8: /* pwm_off */
        e_li(X_A0, 2); e_mv(X_A1, r0); e_li(X_A2, 0); e_li(X_A7, 40); e_ecall();
        break;
    case 9: /* sleep */
        e_mv(X_A0, r0); e_li(X_A7, 8); e_ecall();
        break;
    case 10: /* uptime */
        e_li(X_A7, 9); e_ecall();
        if (d >= 0) e_mv(rreg(d), X_A0);
        break;
    default:
        die("bad native id");
    }
}

static void emit_insn(int fid, int pc)
{
    int op = ir_op(pc);
    int a = ir_a(pc), b = ir_b(pc), c = ir_c(pc), d = ir_d(pc);
    int rd, rs1, rs2;
    switch (op) {
    case R_CONST: e_li(rreg(d), a); break;
    case R_MOV: e_mv(rreg(d), rreg(a)); break;
    case R_ADD: e_add(rreg(d), rreg(a), rreg(b)); break;
    case R_SUB: e_sub(rreg(d), rreg(a), rreg(b)); break;
    case R_MUL: e_mul(rreg(d), rreg(a), rreg(b)); break;
    case R_DIV: e_div(rreg(d), rreg(a), rreg(b)); break;
    case R_MOD: {
        int nz = new_label(), end = new_label();
        e_branch_to(1, rreg(b), X_ZERO, nz);
        e_li(rreg(d), 0);
        e_j(end);
        bind_label(nz);
        e_rem(rreg(d), rreg(a), rreg(b));
        bind_label(end);
        break;
    }
    case R_LT: e_slt(rreg(d), rreg(a), rreg(b)); break;
    case R_GT: e_slt(rreg(d), rreg(b), rreg(a)); break;
    case R_EQ:
        e_xor(rreg(d), rreg(a), rreg(b));
        e_sltiu(rreg(d), rreg(d), 1);
        break;
    case R_NE:
        e_xor(rreg(d), rreg(a), rreg(b));
        e_sltu(rreg(d), X_ZERO, rreg(d));
        break;
    case R_LE:
        e_slt(rreg(d), rreg(b), rreg(a));
        e_xori(rreg(d), rreg(d), 1);
        break;
    case R_GE:
        e_slt(rreg(d), rreg(a), rreg(b));
        e_xori(rreg(d), rreg(d), 1);
        break;
    case R_JZ: cond_jump_zero(rreg(a), pc_label_for(b)); break;
    case R_JMP: e_j(pc_label_for(a)); break;
    case R_RET:
        e_mv(X_A0, rreg(a));
        e_j(func_epi[fid]);
        break;
    case R_HALT:
        e_li(X_A0, 0);
        e_li(X_A7, 0);
        e_ecall();
        break;
    case R_LOADG:
        e_la(X_T3, data_g);
        e_lw(rreg(d), a * 4, X_T3);
        break;
    case R_STOREG:
        e_la(X_T3, data_g);
        e_sw(rreg(b), a * 4, X_T3);
        break;
    case R_LOADA:
    case R_STOREA:
    case R_STOREA_I: {
        int lim = (op == R_LOADA) ? c : d;
        int ok = new_label();
        e_li(X_T3, lim);
        e_branch_to(6, rreg(b), X_T3, ok);  /* bltu idx, len, ok */
        e_call(L_bounds);
        bind_label(ok);
        e_la(X_T4, data_g);
        if (a) {
            e_li(X_T5, a * 4);
            e_add(X_T4, X_T4, X_T5);
        }
        e_slli(X_T3, rreg(b), 2);
        e_add(X_T4, X_T4, X_T3);
        if (op == R_LOADA)
            e_lw(rreg(d), 0, X_T4);
        else if (op == R_STOREA)
            e_sw(rreg(c), 0, X_T4);
        else {
            e_li(X_T6, c);
            e_sw(X_T6, 0, X_T4);
        }
        break;
    }
    case R_PRINT:
        e_mv(X_A0, rreg(a));
        e_call(L_pi);
        break;
    case R_PRINTV:
        e_mv(X_A0, rreg(a));
        e_call(L_pin);
        break;
    case R_PRINTS: {
        int n = 0;
        int off = a;
        if (off >= 0 && off < strsize) {
            while (off + n < strsize && strpool[off + n])
                n++;
        }
        e_la(X_A1, data_strpool);
        if (off) {
            e_li(X_T3, off);
            e_add(X_A1, X_A1, X_T3);
        }
        e_li(X_A2, n);
        e_li(X_A0, 1);
        e_li(X_A7, 1);
        e_ecall();
        break;
    }
    case R_PRINTN:
        e_la(X_A1, data_strpool);
        if (a) {
            e_li(X_T3, a);
            e_add(X_A1, X_A1, X_T3);
        }
        e_li(X_A2, b);
        e_li(X_A0, 1);
        e_li(X_A7, 1);
        e_ecall();
        break;
    case R_PRINTSTR:
        e_mv(X_A0, rreg(a));
        e_call(L_par);
        break;
    case R_ARG:
        e_la(X_T3, data_argbuf);
        e_sw(rreg(b), a * 4, X_T3);
        break;
    case R_CALL:
        e_call(func_label_for(a));
        e_mv(rreg(b), X_A0);
        break;
    case R_LOADL:
        e_lw(rreg(d), 56 + 4 * a, X_SP);
        break;
    case R_STOREL:
        e_sw(rreg(a), 56 + 4 * b, X_SP);
        break;
    case R_INC: e_addi(rreg(d), rreg(a), 1); break;
    case R_DEC: e_addi(rreg(d), rreg(a), -1); break;
    case R_ADDI:
        e_li(X_T3, b);
        e_add(rreg(d), rreg(a), X_T3);
        break;
    case R_SUBI:
        e_li(X_T3, b);
        e_sub(rreg(d), rreg(a), X_T3);
        break;
    case R_MULI:
        e_li(X_T3, b);
        e_mul(rreg(d), rreg(a), X_T3);
        break;
    case R_JLT: cond_jump(5, rreg(a), rreg(b), pc_label_for(c)); break;
    case R_JGE: cond_jump(4, rreg(a), rreg(b), pc_label_for(c)); break;
    case R_JGT: cond_jump(5, rreg(b), rreg(a), pc_label_for(c)); break;
    case R_JLE: cond_jump(4, rreg(b), rreg(a), pc_label_for(c)); break;
    case R_JEQ: cond_jump(1, rreg(a), rreg(b), pc_label_for(c)); break;
    case R_JNE: cond_jump(0, rreg(a), rreg(b), pc_label_for(c)); break;
    case R_JLT_I:
        e_li(X_T3, b);
        cond_jump(5, rreg(a), X_T3, pc_label_for(c));
        break;
    case R_JGE_I:
        e_li(X_T3, b);
        cond_jump(4, rreg(a), X_T3, pc_label_for(c));
        break;
    case R_JGT_I:
        e_li(X_T3, b);
        cond_jump(5, X_T3, rreg(a), pc_label_for(c));
        break;
    case R_JLE_I:
        e_li(X_T3, b);
        cond_jump(4, X_T3, rreg(a), pc_label_for(c));
        break;
    case R_JEQ_I:
        e_li(X_T3, b);
        cond_jump(1, rreg(a), X_T3, pc_label_for(c));
        break;
    case R_JNE_I:
        e_li(X_T3, b);
        cond_jump(0, rreg(a), X_T3, pc_label_for(c));
        break;
    case R_NOP: e_nop(); break;
    case R_LOADD:
        e_slli(X_T3, rreg(a), 2);
        if (b)
            e_addi(X_T3, X_T3, b * 4);
        e_la(X_T4, data_g);
        e_add(X_T3, X_T4, X_T3);
        e_lw(rreg(d), 0, X_T3);
        break;
    case R_STORED:
        e_slli(X_T3, rreg(a), 2);
        if (b)
            e_addi(X_T3, X_T3, b * 4);
        e_la(X_T4, data_g);
        e_add(X_T3, X_T4, X_T3);
        e_sw(rreg(c), 0, X_T3);
        break;
    case R_SHLI: e_slli(rreg(d), rreg(a), b); break;
    case R_SHRI: e_srai(rreg(d), rreg(a), b); break;
    case R_BOUNDS: {
        int ok = new_label();
        e_li(X_T3, b);
        e_branch_to(6, rreg(a), X_T3, ok);
        e_call(L_bounds);
        bind_label(ok);
        break;
    }
    case R_LOADB:
    case R_STOREB: {
        int fail = new_label(), ok = new_label();
        e_branch_to(4, rreg(b), X_ZERO, fail);   /* blt idx, zero, fail */
        e_li(X_T4, a);
        e_add(X_T4, X_T4, rreg(b));
        e_li(X_T3, dsize);
        e_branch_to(6, X_T4, X_T3, ok);          /* bltu base+idx, dsize, ok */
        bind_label(fail);
        e_call(L_bounds);
        bind_label(ok);
        e_la(X_T3, data_arena);
        e_add(X_T4, X_T3, X_T4);
        if (op == R_LOADB)
            e_lbu(rreg(d), 0, X_T4);
        else
            e_sb(rreg(c), 0, X_T4);
        break;
    }
    case R_STRLEN:
        e_mv(X_A0, rreg(a));
        e_call(L_slen);
        e_mv(rreg(d), X_A0);
        break;
    case R_STRCMP:
        e_mv(X_A0, rreg(a));
        e_mv(X_A1, rreg(b));
        e_call(L_scmp);
        e_mv(rreg(d), X_A0);
        break;
    case R_STRCPY:
        e_mv(X_A0, rreg(a));
        e_mv(X_A1, rreg(b));
        e_mv(X_A2, rreg(c));
        e_call(L_scpy);
        e_mv(rreg(d), X_A0);
        break;
    case R_STRCAT:
        e_mv(X_A0, rreg(a));
        e_mv(X_A1, rreg(b));
        e_mv(X_A2, rreg(c));
        e_call(L_scat);
        e_mv(rreg(d), X_A0);
        break;
    case R_ATOI:
        e_mv(X_A0, rreg(a));
        e_call(L_atoi);
        e_mv(rreg(d), X_A0);
        break;
    case R_FMUL:
        e_mv(X_A0, rreg(a));
        e_mv(X_A1, rreg(b));
        e_call(L_fmul);
        e_mv(rreg(d), X_A0);
        break;
    case R_FDIV:
        e_mv(X_A0, rreg(a));
        e_mv(X_A1, rreg(b));
        e_call(L_fdiv);
        e_mv(rreg(d), X_A0);
        break;
    case R_PRINTFIX:
        e_mv(X_A0, rreg(a));
        e_call(L_fix);
        break;
    case R_PRINTFIXV:
        e_mv(X_A0, rreg(a));
        e_call(L_fixn0);
        break;
    case R_PRINTFIXN:
        e_mv(X_A0, rreg(a));
        e_li(X_A1, b);
        e_call(L_fixn);
        break;
    case R_NATIVE:
    case R_NATIVER:
        emit_native(op, a, b, c, op == R_NATIVE ? d : -1,
                    op == R_NATIVER ? d : -1);
        break;
    default:
        die("unknown opcode");
    }
    (void)rs1;
    (void)rs2;
    (void)rd;
}

static void emit_function(int fid)
{
    int frame = frame_for(fid);
    int pc;
    int epi = new_label();
    func_epi[fid] = epi;
    emit_prologue(fid, frame, max_argc[fid]);
    for (pc = func_start[fid]; pc < func_end[fid]; pc++) {
        int l = pc_label_for(pc);
        if (label_off[l] < 0)
            bind_label(l);
        emit_insn(fid, pc);
    }
    emit_epilogue(fid, frame);
}

/* ---- runtime helpers ------------------------------------------------ */

static void emit_string_helpers(void)
{
    int ret, loop;
    bind_label(L_slen);
    ret = new_label();
    loop = new_label();
    e_li(X_T0, 0);
    e_branch_to(4, X_A0, X_ZERO, ret);
    e_la(X_T1, data_arena);
    e_add(X_T1, X_T1, X_A0);
    e_li(X_T2, dsize);
    e_sub(X_T2, X_T2, X_A0);
    bind_label(loop);
    e_branch_to(5, X_T0, X_T2, ret);
    e_lbu(X_T3, 0, X_T1);
    e_branch_to(0, X_T3, X_ZERO, ret);
    e_addi(X_T1, X_T1, 1);
    e_addi(X_T0, X_T0, 1);
    e_j(loop);
    bind_label(ret);
    e_mv(X_A0, X_T0);
    e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));

    bind_label(L_scmp);
    {
        int aempty = new_label(), aok = new_label();
        int bempty = new_label(), bok = new_label();
        int minl = new_label(), mind = new_label();
        int l = new_label(), eq = new_label(), diff = new_label(), less = new_label();
        e_li(X_T3, dsize);
        e_la(X_T1, data_arena);
        e_branch_to(4, X_A0, X_ZERO, aempty);
        e_branch_to(5, X_A0, X_T3, aempty);
        e_add(X_T1, X_T1, X_A0);
        e_sub(X_T4, X_T3, X_A0);
        e_j(aok);
        bind_label(aempty);
        e_la(X_T1, rodata_empty);
        e_li(X_T4, 1);
        bind_label(aok);
        e_la(X_T2, data_arena);
        e_branch_to(4, X_A1, X_ZERO, bempty);
        e_branch_to(5, X_A1, X_T3, bempty);
        e_add(X_T2, X_T2, X_A1);
        e_sub(X_T5, X_T3, X_A1);
        e_j(bok);
        bind_label(bempty);
        e_la(X_T2, rodata_empty);
        e_li(X_T5, 1);
        bind_label(bok);
        e_branch_to(4, X_T4, X_T5, minl);
        e_mv(X_T3, X_T5);
        e_j(mind);
        bind_label(minl);
        e_mv(X_T3, X_T4);
        bind_label(mind);
        e_li(X_T0, 0);
        bind_label(l);
        e_branch_to(5, X_T0, X_T3, eq);
        e_lbu(X_T4, 0, X_T1);
        e_lbu(X_T5, 0, X_T2);
        e_branch_to(1, X_T4, X_T5, diff);
        e_branch_to(0, X_T4, X_ZERO, eq);
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T2, X_T2, 1);
        e_addi(X_T0, X_T0, 1);
        e_j(l);
        bind_label(diff);
        e_branch_to(6, X_T4, X_T5, less);
        e_li(X_A0, 1);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
        bind_label(less);
        e_li(X_A0, -1);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
        bind_label(eq);
        e_li(X_A0, 0);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }

    bind_label(L_scpy);
    {
        int r = new_label(), z = new_label(), src = new_label();
        int l = new_label(), term = new_label(), nn = new_label();
        e_addi(X_SP, X_SP, -32);
        e_sw(X_RA, 28, X_SP);
        e_sw(X_A0, 0, X_SP);
        e_sw(X_A1, 4, X_SP);
        e_sw(X_A2, 8, X_SP);
        e_mv(X_A0, X_A1);
        e_call(L_slen);
        e_mv(X_A3, X_A0);
        e_lw(X_A0, 0, X_SP);
        e_lw(X_A1, 4, X_SP);
        e_lw(X_A2, 8, X_SP);
        e_branch_to(4, X_A0, X_ZERO, r);
        e_li(X_T0, dsize);
        e_branch_to(5, X_A0, X_T0, r);
        e_branch_to(5, X_ZERO, X_A2, r);
        e_addi(X_T4, X_A2, -1);
        e_branch_to(4, X_T4, X_A3, nn);
        e_mv(X_T4, X_A3);
        bind_label(nn);
        e_la(X_T1, data_arena);
        e_branch_to(4, X_A1, X_ZERO, z);
        e_add(X_T1, X_T1, X_A1);
        e_j(src);
        bind_label(z);
        e_li(X_T4, 0);
        bind_label(src);
        e_la(X_T2, data_arena);
        e_add(X_T2, X_T2, X_A0);
        e_la(X_T6, data_arena);
        e_li(X_T5, dsize);
        e_add(X_T6, X_T6, X_T5);
        e_li(X_T0, 0);
        bind_label(l);
        e_branch_to(5, X_T0, X_T4, term);
        e_branch_to(7, X_T2, X_T6, r);
        e_lbu(X_T5, 0, X_T1);
        e_sb(X_T5, 0, X_T2);
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T2, X_T2, 1);
        e_addi(X_T0, X_T0, 1);
        e_j(l);
        bind_label(term);
        e_branch_to(7, X_T2, X_T6, r);
        e_sb(X_ZERO, 0, X_T2);
        bind_label(r);
        e_mv(X_A0, X_A3);
        e_lw(X_RA, 28, X_SP);
        e_addi(X_SP, X_SP, 32);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }

    bind_label(L_scat);
    {
        int r = new_label(), z = new_label(), src = new_label();
        int l = new_label(), term = new_label(), nn = new_label();
        e_addi(X_SP, X_SP, -48);
        e_sw(X_RA, 44, X_SP);
        e_sw(X_A0, 0, X_SP);
        e_sw(X_A1, 4, X_SP);
        e_sw(X_A2, 8, X_SP);
        e_call(L_slen);
        e_mv(X_A3, X_A0);
        e_lw(X_A1, 4, X_SP);
        e_mv(X_A0, X_A1);
        e_call(L_slen);
        e_mv(X_A4, X_A0);
        e_lw(X_A0, 0, X_SP);
        e_lw(X_A1, 4, X_SP);
        e_lw(X_A2, 8, X_SP);
        e_add(X_A5, X_A3, X_A4);
        e_branch_to(4, X_A0, X_ZERO, r);
        e_li(X_T0, dsize);
        e_branch_to(5, X_A0, X_T0, r);
        e_addi(X_T4, X_A3, 1);
        e_branch_to(5, X_T4, X_A2, r);
        e_addi(X_T4, X_A2, -1);
        e_sub(X_T4, X_T4, X_A3);
        e_branch_to(4, X_T4, X_A4, nn);
        e_mv(X_T4, X_A4);
        bind_label(nn);
        e_la(X_T1, data_arena);
        e_branch_to(4, X_A1, X_ZERO, z);
        e_add(X_T1, X_T1, X_A1);
        e_j(src);
        bind_label(z);
        e_li(X_T4, 0);
        bind_label(src);
        e_la(X_T2, data_arena);
        e_add(X_T2, X_T2, X_A0);
        e_add(X_T2, X_T2, X_A3);
        e_la(X_T6, data_arena);
        e_li(X_T5, dsize);
        e_add(X_T6, X_T6, X_T5);
        e_li(X_T0, 0);
        bind_label(l);
        e_branch_to(5, X_T0, X_T4, term);
        e_branch_to(7, X_T2, X_T6, r);
        e_lbu(X_T5, 0, X_T1);
        e_sb(X_T5, 0, X_T2);
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T2, X_T2, 1);
        e_addi(X_T0, X_T0, 1);
        e_j(l);
        bind_label(term);
        e_branch_to(7, X_T2, X_T6, r);
        e_sb(X_ZERO, 0, X_T2);
        bind_label(r);
        e_mv(X_A0, X_A5);
        e_lw(X_RA, 44, X_SP);
        e_addi(X_SP, X_SP, 48);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }

    bind_label(L_atoi);
    {
        int zero = new_label(), skip = new_label(), s1 = new_label(), sign = new_label();
        int plus = new_label(), digits = new_label(), l = new_label();
        int fin = new_label(), r = new_label();
        e_branch_to(4, X_A0, X_ZERO, zero);
        e_la(X_T1, data_arena);
        e_add(X_T1, X_T1, X_A0);
        e_li(X_T2, dsize);
        e_sub(X_T2, X_T2, X_A0);
        e_li(X_T0, 0);
        e_li(X_T6, 0);
        bind_label(skip);
        e_branch_to(5, X_T0, X_T2, zero);
        e_lbu(X_T3, 0, X_T1);
        e_li(X_T4, 32);
        e_branch_to(0, X_T3, X_T4, s1);
        e_li(X_T4, 9);
        e_branch_to(1, X_T3, X_T4, sign);
        bind_label(s1);
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T0, X_T0, 1);
        e_j(skip);
        bind_label(sign);
        e_li(X_T4, 45);
        e_branch_to(1, X_T3, X_T4, plus);
        e_li(X_T6, 1);
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T0, X_T0, 1);
        e_j(digits);
        bind_label(plus);
        e_li(X_T4, 43);
        e_branch_to(1, X_T3, X_T4, digits);
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T0, X_T0, 1);
        bind_label(digits);
        e_li(X_T5, 0);
        bind_label(l);
        e_branch_to(5, X_T0, X_T2, fin);
        e_lbu(X_T3, 0, X_T1);
        e_li(X_T4, 48);
        e_branch_to(4, X_T3, X_T4, fin);
        e_li(X_T4, 57);
        e_branch_to(4, X_T4, X_T3, fin);
        e_addi(X_T3, X_T3, -48);
        e_li(X_T4, 10);
        e_mul(X_T5, X_T5, X_T4);
        e_add(X_T5, X_T5, X_T3);
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T0, X_T0, 1);
        e_j(l);
        bind_label(fin);
        e_mv(X_A0, X_T5);
        e_branch_to(0, X_T6, X_ZERO, r);
        e_sub(X_A0, X_ZERO, X_A0);
        bind_label(r);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
        bind_label(zero);
        e_li(X_A0, 0);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }
}

static void emit_float_helpers(void)
{
    bind_label(L_fmul);
    e_xor(X_T0, X_A0, X_A1);
    e_srai(X_T0, X_T0, 31);
    e_srai(X_T1, X_A0, 31);
    e_xor(X_T2, X_A0, X_T1);
    e_sub(X_T2, X_T2, X_T1);
    e_srai(X_T1, X_A1, 31);
    e_xor(X_T3, X_A1, X_T1);
    e_sub(X_T3, X_T3, X_T1);
    e_slli(X_T4, X_T2, 16); e_srli(X_T4, X_T4, 16);
    e_srli(X_T5, X_T2, 16);
    e_slli(X_T6, X_T3, 16); e_srli(X_T6, X_T6, 16);
    e_srli(X_A2, X_T3, 16);
    e_mul(X_A3, X_T4, X_T6);
    e_mul(X_A4, X_T4, X_A2);
    e_mul(X_A5, X_T5, X_T6);
    e_mul(X_A6, X_T5, X_A2);
    e_srli(X_A7, X_A3, 16);
    e_slli(X_T4, X_A4, 16); e_srli(X_T4, X_T4, 16); e_add(X_A7, X_A7, X_T4);
    e_slli(X_T4, X_A5, 16); e_srli(X_T4, X_T4, 16); e_add(X_A7, X_A7, X_T4);
    e_srli(X_T5, X_A4, 16); e_add(X_A6, X_A6, X_T5);
    e_srli(X_T5, X_A5, 16); e_add(X_A6, X_A6, X_T5);
    e_srli(X_T5, X_A7, 16); e_add(X_A6, X_A6, X_T5);
    e_slli(X_A6, X_A6, 16);
    e_slli(X_A7, X_A7, 16); e_srli(X_A7, X_A7, 16);
    e_or(X_A0, X_A6, X_A7);
    e_xor(X_A0, X_A0, X_T0);
    e_sub(X_A0, X_A0, X_T0);
    e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));

    bind_label(L_fdiv);
    {
        int nz = new_label(), negmax = new_label(), clamp = new_label();
        int loop = new_label(), done = new_label(), sub = new_label(), nosub = new_label();
        int posmax = new_label();
        e_branch_to(1, X_A1, X_ZERO, nz);
        e_branch_to(4, X_A0, X_ZERO, negmax);
        e_li(X_A0, 2147483647);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
        bind_label(negmax);
        e_li(X_A0, -2147483647 - 1);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
        bind_label(nz);
        e_xor(X_T0, X_A0, X_A1);
        e_srai(X_T0, X_T0, 31);
        e_srai(X_T1, X_A0, 31);
        e_xor(X_T2, X_A0, X_T1);
        e_sub(X_T2, X_T2, X_T1);
        e_srai(X_T1, X_A1, 31);
        e_xor(X_T3, X_A1, X_T1);
        e_sub(X_T3, X_T3, X_T1);
        e_divu(X_T4, X_T2, X_T3);
        e_remu(X_T5, X_T2, X_T3);
        e_li(X_T6, 32767);
        e_branch_to(4, X_T6, X_T4, clamp);
        e_li(X_A2, 0);
        e_li(X_A3, 0);
        bind_label(loop);
        e_li(X_A4, 16);
        e_branch_to(5, X_A3, X_A4, done);
        e_srli(X_A5, X_T5, 31);
        e_slli(X_T5, X_T5, 1);
        e_branch_to(1, X_A5, X_ZERO, sub);
        e_branch_to(6, X_T5, X_T3, nosub);
        bind_label(sub);
        e_sub(X_T5, X_T5, X_T3);
        e_li(X_A4, 15);
        e_sub(X_A4, X_A4, X_A3);
        e_li(X_A5, 1);
        e_sll(X_A5, X_A5, X_A4);
        e_or(X_A2, X_A2, X_A5);
        bind_label(nosub);
        e_addi(X_A3, X_A3, 1);
        e_j(loop);
        bind_label(done);
        e_slli(X_T4, X_T4, 16);
        e_or(X_A0, X_T4, X_A2);
        e_xor(X_A0, X_A0, X_T0);
        e_sub(X_A0, X_A0, X_T0);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
        bind_label(clamp);
        e_branch_to(0, X_T0, X_ZERO, posmax);
        e_li(X_A0, -2147483647 - 1);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
        bind_label(posmax);
        e_li(X_A0, 2147483647);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }

    bind_label(L_fix);
    e_li(X_A1, 1);
    {
        int body = new_label();
        e_j(body);
        bind_label(L_fixn0);
        e_li(X_A1, 0);
        bind_label(body);
    }
    {
        int pos = new_label(), dig = new_label(), trim = new_label();
        int emit = new_label(), done = new_label();
        e_addi(X_SP, X_SP, -32);
        e_sw(X_RA, 28, X_SP);
        e_sw(X_A1, 4, X_SP);
        e_sw(X_A0, 0, X_SP);
        e_lw(X_T0, 0, X_SP);
        e_branch_to(5, X_T0, X_ZERO, pos);
        e_li(X_T0, 45);
        e_sb(X_T0, 16, X_SP);
        e_li(X_A0, 1); e_addi(X_A1, X_SP, 16); e_li(X_A2, 1); e_li(X_A7, 1); e_ecall();
        e_lw(X_T0, 0, X_SP);
        e_sub(X_T0, X_ZERO, X_T0);
        e_sw(X_T0, 0, X_SP);
        bind_label(pos);
        e_lw(X_T0, 0, X_SP);
        e_srli(X_A0, X_T0, 16);
        e_call(L_pin);
        e_li(X_T0, 46);
        e_sb(X_T0, 16, X_SP);
        e_li(X_A0, 1); e_addi(X_A1, X_SP, 16); e_li(X_A2, 1); e_li(X_A7, 1); e_ecall();
        e_lw(X_T0, 0, X_SP);
        e_slli(X_T0, X_T0, 16); e_srli(X_T0, X_T0, 16);
        e_li(X_T1, 10000);
        e_mul(X_T0, X_T0, X_T1);
        e_li(X_T1, 32768);
        e_add(X_T0, X_T0, X_T1);
        e_srli(X_T0, X_T0, 16);
        e_sw(X_T0, 8, X_SP);
        e_li(X_T1, 10);
        e_li(X_T3, 3);
        bind_label(dig);
        e_remu(X_T4, X_T0, X_T1);
        e_addi(X_T4, X_T4, 48);
        e_addi(X_T5, X_SP, 12);
        e_add(X_T5, X_T5, X_T3);
        e_sb(X_T4, 0, X_T5);
        e_divu(X_T0, X_T0, X_T1);
        e_addi(X_T3, X_T3, -1);
        e_branch_to(5, X_T3, X_ZERO, dig);
        e_li(X_T3, 4);
        bind_label(trim);
        e_li(X_T4, 1);
        e_branch_to(5, X_T4, X_T3, emit);
        e_addi(X_T5, X_SP, 12);
        e_add(X_T5, X_T5, X_T3);
        e_addi(X_T5, X_T5, -1);
        e_lbu(X_T4, 0, X_T5);
        e_li(X_T6, 48);
        e_branch_to(1, X_T4, X_T6, emit);
        e_addi(X_T3, X_T3, -1);
        e_j(trim);
        bind_label(emit);
        e_addi(X_A1, X_SP, 12);
        e_mv(X_A2, X_T3);
        e_li(X_A0, 1); e_li(X_A7, 1); e_ecall();
        e_lw(X_T4, 4, X_SP);
        e_branch_to(0, X_T4, X_ZERO, done);
        e_li(X_T0, 10);
        e_sb(X_T0, 16, X_SP);
        e_li(X_A0, 1); e_addi(X_A1, X_SP, 16); e_li(X_A2, 1); e_li(X_A7, 1); e_ecall();
        bind_label(done);
        e_lw(X_RA, 28, X_SP);
        e_addi(X_SP, X_SP, 32);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }

    bind_label(L_fixn);
    {
        int d0 = new_label(), d4 = new_label(), dok = new_label();
        int pos = new_label(), nocarry = new_label(), nosign = new_label();
        int done = new_label(), digits = new_label();
        e_addi(X_SP, X_SP, -48);
        e_sw(X_RA, 44, X_SP);
        e_sw(X_A0, 0, X_SP);
        e_sw(X_A1, 4, X_SP);
        e_branch_to(4, X_A1, X_ZERO, d0);
        e_li(X_T0, 4);
        e_branch_to(4, X_T0, X_A1, d4);
        e_j(dok);
        bind_label(d0);
        e_li(X_A1, 0);
        e_sw(X_A1, 4, X_SP);
        e_j(dok);
        bind_label(d4);
        e_li(X_A1, 4);
        e_sw(X_A1, 4, X_SP);
        bind_label(dok);
        e_lw(X_T0, 0, X_SP);
        e_li(X_T5, 0);
        e_branch_to(5, X_T0, X_ZERO, pos);
        e_li(X_T5, 1);
        e_sub(X_T0, X_ZERO, X_T0);
        e_sw(X_T0, 0, X_SP);
        bind_label(pos);
        e_sw(X_T5, 28, X_SP);
        e_lw(X_T0, 0, X_SP);
        e_slli(X_T0, X_T0, 16); e_srli(X_T0, X_T0, 16);
        e_lw(X_A1, 4, X_SP);
        e_la(X_T1, rodata_p10);
        e_slli(X_T2, X_A1, 2);
        e_add(X_T1, X_T1, X_T2);
        e_lw(X_T2, 0, X_T1);
        e_mul(X_T0, X_T0, X_T2);
        e_li(X_T3, 32768);
        e_add(X_T0, X_T0, X_T3);
        e_srli(X_T0, X_T0, 16);
        e_lw(X_T4, 0, X_SP);
        e_srli(X_T4, X_T4, 16);
        e_branch_to(4, X_T0, X_T2, nocarry);
        e_sub(X_T0, X_T0, X_T2);
        e_addi(X_T4, X_T4, 1);
        bind_label(nocarry);
        e_sw(X_T4, 8, X_SP);
        e_sw(X_T0, 12, X_SP);
        e_lw(X_T5, 28, X_SP);
        e_branch_to(0, X_T5, X_ZERO, nosign);
        e_li(X_T0, 45);
        e_sb(X_T0, 24, X_SP);
        e_li(X_A0, 1); e_addi(X_A1, X_SP, 24); e_li(X_A2, 1); e_li(X_A7, 1); e_ecall();
        bind_label(nosign);
        e_lw(X_A0, 8, X_SP);
        e_call(L_pin);
        e_lw(X_T3, 4, X_SP);
        e_branch_to(0, X_T3, X_ZERO, done);
        e_li(X_T0, 46);
        e_sb(X_T0, 24, X_SP);
        e_li(X_A0, 1); e_addi(X_A1, X_SP, 24); e_li(X_A2, 1); e_li(X_A7, 1); e_ecall();
        e_lw(X_T3, 4, X_SP);
        e_addi(X_T3, X_T3, -1);
        e_lw(X_T0, 12, X_SP);
        e_li(X_T1, 10);
        bind_label(digits);
        e_remu(X_T2, X_T0, X_T1);
        e_addi(X_T2, X_T2, 48);
        e_addi(X_T4, X_SP, 16);
        e_add(X_T4, X_T4, X_T3);
        e_sb(X_T2, 0, X_T4);
        e_divu(X_T0, X_T0, X_T1);
        e_addi(X_T3, X_T3, -1);
        e_branch_to(5, X_T3, X_ZERO, digits);
        e_lw(X_A2, 4, X_SP);
        e_addi(X_A1, X_SP, 16);
        e_li(X_A0, 1); e_li(X_A7, 1); e_ecall();
        bind_label(done);
        e_lw(X_RA, 44, X_SP);
        e_addi(X_SP, X_SP, 48);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }
}

static void emit_helpers(void)
{
    bind_label(L_bounds);
    e_la(X_A1, rodata_msg);
    e_li(X_A2, 7);
    e_li(X_A0, 1);
    e_li(X_A7, 1);
    e_ecall();
    e_li(X_A0, 1);
    e_li(X_A7, 0);
    e_ecall();

    bind_label(L_pi);
    e_li(X_A1, 1);
    e_j(L_picommon);
    bind_label(L_pin);
    e_li(X_A1, 0);
    bind_label(L_picommon);
    {
        int pos = new_label(), digit = new_label(), sign = new_label();
        int nowrap = new_label();
        e_addi(X_SP, X_SP, -32);
        e_sw(X_RA, 28, X_SP);
        e_sw(X_A1, 24, X_SP);
        e_mv(X_T0, X_A0);
        e_addi(X_T1, X_SP, 20);
        e_li(X_T2, 0);
        e_branch_to(5, X_T0, X_ZERO, pos);      /* bge t0, zero, pos */
        e_li(X_T2, 1);
        e_sub(X_T0, X_ZERO, X_T0);
        bind_label(pos);
        e_li(X_T3, 10);
        e_mv(X_T4, X_T1);
        bind_label(digit);
        e_remu(X_T5, X_T0, X_T3);
        e_addi(X_T5, X_T5, 48);
        e_addi(X_T4, X_T4, -1);
        e_sb(X_T5, 0, X_T4);
        e_divu(X_T0, X_T0, X_T3);
        e_branch_to(1, X_T0, X_ZERO, digit);    /* bne t0, zero, digit */
        e_branch_to(0, X_T2, X_ZERO, sign);     /* beq t2, zero, sign */
        e_addi(X_T4, X_T4, -1);
        e_li(X_T5, 45);
        e_sb(X_T5, 0, X_T4);
        bind_label(sign);
        e_lw(X_T6, 24, X_SP);
        e_branch_to(0, X_T6, X_ZERO, nowrap);   /* beq t6, zero, nowrap */
        e_li(X_T5, 10);
        e_sb(X_T5, 0, X_T1);
        e_addi(X_T1, X_T1, 1);
        bind_label(nowrap);
        e_sub(X_A2, X_T1, X_T4);
        e_mv(X_A1, X_T4);
        e_li(X_A0, 1);
        e_li(X_A7, 1);
        e_ecall();
        e_lw(X_RA, 28, X_SP);
        e_addi(X_SP, X_SP, 32);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }

    bind_label(L_par);
    {
        int ret = new_label(), loop = new_label(), done = new_label();
        e_branch_to(4, X_A0, X_ZERO, ret);      /* blt a0, zero, ret */
        e_addi(X_SP, X_SP, -16);
        e_sw(X_RA, 12, X_SP);
        e_la(X_T1, data_arena);
        e_add(X_T1, X_T1, X_A0);
        e_li(X_T2, dsize);
        e_sub(X_T2, X_T2, X_A0);
        e_mv(X_T3, X_T1);
        e_li(X_T4, 0);
        bind_label(loop);
        e_branch_to(5, X_T4, X_T2, done);       /* bge t4, t2, done */
        e_lbu(X_T5, 0, X_T1);
        e_branch_to(0, X_T5, X_ZERO, done);     /* beq t5, zero, done */
        e_addi(X_T1, X_T1, 1);
        e_addi(X_T4, X_T4, 1);
        e_j(loop);
        bind_label(done);
        e_mv(X_A1, X_T3);
        e_mv(X_A2, X_T4);
        e_li(X_A0, 1);
        e_li(X_A7, 1);
        e_ecall();
        e_lw(X_RA, 12, X_SP);
        e_addi(X_SP, X_SP, 16);
        bind_label(ret);
        e32(enc_i(0, X_ZERO, X_RA, 0, 0x67));
    }
    emit_string_helpers();
    emit_float_helpers();
}

/* ---- layout, ELF and LZ4TOS ----------------------------------------- */

static void emit_rodata(void)
{
    static const uint8_t msg[] = "bounds\n";
    static const uint8_t zero = 0;
    static const int32_t p10[5] = { 1, 10, 100, 1000, 10000 };
    int i;

    while (text_len & 3)
        e_bytes(&zero, 1);
    bind_label(rodata_msg);
    e_bytes(msg, 7);

    while (text_len & 3)
        e_bytes(&zero, 1);
    bind_label(rodata_empty);
    e_bytes(&zero, 1);
    bind_label(data_strpool);
    if (strsize > 0)
        e_bytes(strpool, strsize);
    e_bytes(&zero, 1);

    while (text_len & 3)
        e_bytes(&zero, 1);
    bind_label(rodata_p10);
    for (i = 0; i < 5; i++)
        e32((uint32_t)p10[i]);
}

static void write_elf_header(int data_off, int data_filesz, int data_memsz)
{
    uint8_t h[TEXT_OFF];
    memset(h, 0, sizeof h);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
    h[4] = 1; h[5] = 1; h[6] = 1;
    put16(h + 16, 3);                      /* ET_DYN */
    put16(h + 18, 243);                    /* EM_RISCV */
    put32(h + 20, 1);
    put32(h + 24, 0);                      /* entry */
    put32(h + 28, 52);                     /* phoff */
    put32(h + 32, 0);                      /* shoff */
    put32(h + 36, 0);
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
    uint8_t hdr[256];
    int hn = 0;
    if (n < 15) {
        hdr[hn++] = (uint8_t)(n << 4);
    } else {
        int rem = n - 15;
        hdr[hn++] = 0xf0;
        while (rem >= 255) {
            hdr[hn++] = 255;
            rem -= 255;
        }
        hdr[hn++] = (uint8_t)rem;
    }
    return write_all_fd(fd, hdr, hn);
}

static int load_rir(const char *path)
{
    int fd = open(path);
    int n = 0;
    if (fd < 0)
        return -1;
    for (;;) {
        int r;
        if (n >= RIR_MAX) {
            close(fd);
            return -1;
        }
        r = read(fd, rir_buf + n, RIR_MAX - n);
        if (r <= 0)
            break;
        n += r;
    }
    close(fd);
    rir_len = n;
    return n;
}

static void parse_rir(void)
{
    uint32_t magic;
    if (rir_len < 28)
        die("short RIR2 header");
    magic = get32(rir_buf);
    if (magic != 0x32524952u)
        die("not a RIR2 image");
    nir = (int)get32(rir_buf + 4);
    gsize = (int)get32(rir_buf + 8);
    entry_pc = (int)get32(rir_buf + 12);
    nfunc = (int)get32(rir_buf + 16);
    strsize = (int)get32(rir_buf + 20);
    dsize = (int)get32(rir_buf + 24);
    if (nir <= 0 || gsize < 0 || nfunc <= 0 || strsize < 0 || dsize < 0)
        die("bad RIR2 header");
    if (28 + nir * 20 + nfunc * 4 + strsize + dsize > rir_len)
        die("truncated RIR2 image");
    code = rir_buf + 28;
    strpool = code + nir * 20 + nfunc * 4;
    arena = strpool + strsize;
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

static void emit_body(void)
{
    int i, halt;
    text_len = 0;
    halt = new_label();
    e_call(func_label_for(main_fid));
    e_li(X_A7, 0);
    e_ecall();
    bind_label(halt);
    e_j(halt);
    for (i = 0; i < nfunc; i++)
        emit_function(func_order[i]);
    emit_helpers();
    emit_rodata();
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
    L_bounds = new_label();
    L_pi = new_label();
    L_pin = new_label();
    L_picommon = new_label();
    L_par = new_label();
    L_scmp = new_label();
    L_slen = new_label();
    L_scpy = new_label();
    L_scat = new_label();
    L_atoi = new_label();
    L_fmul = new_label();
    L_fdiv = new_label();
    L_fix = new_label();
    L_fixn0 = new_label();
    L_fixn = new_label();
    rodata_msg = new_label();
    rodata_p10 = new_label();
    rodata_empty = new_label();

    analyze();
    label_base = label_next;

    emit_pass = 1;
    emit_body();
    text_final = text_len;

    data_va = align16(text_final);
    data_off = align16(TEXT_OFF + text_final);
    arena_bytes = (dsize + 3) & ~3;
    data_filesz = arena_bytes;
    g_bytes = gsize * 4;
    if (g_bytes <= 0)
        g_bytes = 4;
    argbuf_off = (g_bytes + 3) & ~3;
    pwm_off = argbuf_off + 64;
    bss = pwm_off + 12;

    label_off[data_arena] = data_va;
    label_off[data_g] = data_va + arena_bytes;
    label_off[data_argbuf] = data_va + arena_bytes + argbuf_off;
    label_off[data_pwmreq] = data_va + arena_bytes + pwm_off;
    data_bss_size = bss;

    out_len = data_off + data_filesz;
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
    emit_body();
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
    puts(" tos\n");
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
