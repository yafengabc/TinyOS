/* Thumb-2 backend (ARMv7-M / ARMv8-M Mainline).  Included by rcc.c when the
 * target is Cortex-M3/M33.  Stage 1 of the port: integer core, calls,
 * globals/arrays, print int and bounds.  Strings, floats and the native
 * syscalls are still refused with a diagnostic so a half-built backend cannot
 * pretend to have compiled a program.
 *
 * ABI chosen here: the first seven RIR registers live in r4..r10 (callee
 * saved), RIR 7..15 live in frame slots, r0..r3/r12 are scratch and r0 is the
 * return value.  Arguments travel through the same `data_argbuf` word array
 * the RV32 backend uses, so no argument registers have to survive a nested
 * call.
 */
#if defined(__arm__) || defined(RCC_ARCH_THUMB2)

#define ARM_SCRATCH0 0
#define ARM_SCRATCH1 1
#define ARM_SCRATCH2 2
#define ARM_SCRATCH3 3
#define ARM_TMP      12
#define ARM_SP       13
#define ARM_LR       14
#define ARM_PC       15

#define VREG_IN_REG 7          /* RIR 0..6 -> r4..r10                  */
#define SLOT_BYTES 40          /* 9 slots (RIR 7..15) + alignment      */

/* ---- instruction encoders ------------------------------------------- */

static void e16(uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    e_bytes(b, 2);
}

static void e_add_imm(int rd, int rn, int imm);
static void e_sub_imm(int rd, int rn, int imm);

static void e32h(uint16_t a, uint16_t b)
{
    e16(a);
    e16(b);
}

static void e_movw(int rd, uint32_t v)
{
    e32h((uint16_t)(0xF240 | (((v >> 11) & 1) << 10) | ((v >> 12) & 0xF)),
         (uint16_t)((((v >> 8) & 7) << 12) | ((rd & 0xF) << 8) | (v & 0xFF)));
}

static void e_movt(int rd, uint32_t v)
{
    e32h((uint16_t)(0xF2C0 | (((v >> 27) & 1) << 10) | ((v >> 28) & 0xF)),
         (uint16_t)((((v >> 24) & 7) << 12) | ((rd & 0xF) << 8) |
                    ((v >> 16) & 0xFF)));
}

static void e_li(int rd, uint32_t v)
{
    e_movw(rd, v);
    if (v > 0xFFFFu)
        e_movt(rd, v);
}

static void e_mov_w(int rd, int rm)
{
    e32h((uint16_t)(0xEA4F), (uint16_t)((rd << 8) | (rm & 0xF)));
}

static void e_add_reg(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xEB00 | (rn & 0xF)), (uint16_t)((rd << 8) | (rm & 0xF)));
}

static void e_sub_reg(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xEBA0 | (rn & 0xF)), (uint16_t)((rd << 8) | (rm & 0xF)));
}

static void e_add_imm(int rd, int rn, int imm)
{
    if (imm < 0) {
        e_sub_imm(rd, rn, -imm);
        return;
    }
    if (imm == 0) {
        e_mov_w(rd, rn);
        return;
    }
    e32h((uint16_t)(0xF100 | (((imm >> 11) & 1) << 10) | (rn & 0xF)),
         (uint16_t)((((imm >> 8) & 7) << 12) | ((rd & 0xF) << 8) | (imm & 0xFF)));
}

static void e_sub_imm(int rd, int rn, int imm)
{
    if (imm < 0) {
        e_add_imm(rd, rn, -imm);
        return;
    }
    if (imm == 0) {
        e_mov_w(rd, rn);
        return;
    }
    e32h((uint16_t)(0xF1A0 | (((imm >> 11) & 1) << 10) | (rn & 0xF)),
         (uint16_t)((((imm >> 8) & 7) << 12) | ((rd & 0xF) << 8) | (imm & 0xFF)));
}

static void e_subs_imm(int rd, int rn, int imm)
{
    e32h((uint16_t)(0xF1B0 | (((imm >> 11) & 1) << 10) | (rn & 0xF)),
         (uint16_t)((((imm >> 8) & 7) << 12) | ((rd & 0xF) << 8) | (imm & 0xFF)));
}

static void e_mul(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xFB00 | (rn & 0xF)),
         (uint16_t)(0xF000 | ((rd & 0xF) << 8) | (rm & 0xF)));
}

static void e_sdiv(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xFB90 | (rn & 0xF)),
         (uint16_t)(0xF0F0 | ((rd & 0xF) << 8) | (rm & 0xF)));
}

static void e_udiv(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xFBB0 | (rn & 0xF)),
         (uint16_t)(0xF0F0 | ((rd & 0xF) << 8) | (rm & 0xF)));
}

static void e_mls(int rd, int rn, int rm, int ra)
{
    e32h((uint16_t)(0xFB00 | (rn & 0xF)),
         (uint16_t)(((ra & 0xF) << 12) | ((rd & 0xF) << 8) | 0x10 | (rm & 0xF)));
}

static void e_cmp(int rn, int rm)
{
    e32h((uint16_t)(0xEBB0 | (rn & 0xF)), (uint16_t)(0x0F00 | (rm & 0xF)));
}

static void e_cmp_zero(int rn)
{
    e32h((uint16_t)(0xF1B0 | (rn & 0xF)), 0x0F00);
}

static void e_shift_imm(int rd, int rm, int sh, int type)
{
    e32h(0xEA4F,
         (uint16_t)(((sh >> 2) << 12) | ((rd & 0xF) << 8) |
                    ((sh & 3) << 6) | (type << 4) | (rm & 0xF)));
}

static void e_lsl_imm(int rd, int rm, int sh) { e_shift_imm(rd, rm, sh, 0); }
static void e_lsr_imm(int rd, int rm, int sh) { e_shift_imm(rd, rm, sh, 1); }
static void e_asr_imm(int rd, int rm, int sh) { e_shift_imm(rd, rm, sh, 2); }

static void e_ldr_imm(int rd, int rn, int off)
{
    e32h((uint16_t)(0xF8D0 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | (off & 0xFFF)));
}

static void e_str_imm(int rd, int rn, int off)
{
    e32h((uint16_t)(0xF8C0 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | (off & 0xFFF)));
}

static void e_ldrb_imm(int rd, int rn, int off)
{
    e32h((uint16_t)(0xF890 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | (off & 0xFFF)));
}

static void e_strb_imm(int rd, int rn, int off)
{
    e32h((uint16_t)(0xF880 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | (off & 0xFFF)));
}

static void e_ldrb_reg(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xF810 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | (rm & 0xF)));
}

static void e_strb_reg(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xF800 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | (rm & 0xF)));
}

static void e_ldr_reg_shift(int rd, int rn, int rm, int sh)
{
    e32h((uint16_t)(0xF850 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | ((sh & 3) << 4) | (rm & 0xF)));
}

static void e_str_reg_shift(int rd, int rn, int rm, int sh)
{
    e32h((uint16_t)(0xF840 | (rn & 0xF)),
         (uint16_t)(((rd & 0xF) << 12) | ((sh & 3) << 4) | (rm & 0xF)));
}

static void e_orr(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xEA40 | (rn & 0xF)), (uint16_t)((rd << 8) | (rm & 0xF)));
}

static void e_eor(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xEA80 | (rn & 0xF)), (uint16_t)((rd << 8) | (rm & 0xF)));
}

static void e_and(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xEA00 | (rn & 0xF)), (uint16_t)((rd << 8) | (rm & 0xF)));
}

static void e_lsl_reg(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xFA00 | (rn & 0xF)),
         (uint16_t)(0xF000 | ((rd & 0xF) << 8) | (rm & 0xF)));
}

static void e_lsr_reg(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xFA20 | (rn & 0xF)),
         (uint16_t)(0xF000 | ((rd & 0xF) << 8) | (rm & 0xF)));
}

static void e_asr_reg(int rd, int rn, int rm)
{
    e32h((uint16_t)(0xFA40 | (rn & 0xF)),
         (uint16_t)(0xF000 | ((rd & 0xF) << 8) | (rm & 0xF)));
}

static void e_smull(int lo, int hi, int rn, int rm)
{
    e32h((uint16_t)(0xFB80 | (rn & 0xF)),
         (uint16_t)(((lo & 0xF) << 12) | ((hi & 0xF) << 8) | (rm & 0xF)));
}

static void e_bic_imm(int rd, int rn, int imm)
{
    e32h((uint16_t)(0xF020 | (((imm >> 11) & 1) << 10) | (rn & 0xF)),
         (uint16_t)((((imm >> 8) & 7) << 12) | ((rd & 0xF) << 8) | (imm & 0xFF)));
}

static void e_push(void)  { e32h(0xE92D, 0x4FF0); }   /* {r4-r11, lr} */
static void e_pop(void)   { e32h(0xE8BD, 0x8FF0); }   /* {r4-r11, pc} */
static void e_svc0(void)  { e16(0xDF00); }
static void e_movs_imm8(int rd, int imm) { e16((uint16_t)(0x2000 | ((rd & 7) << 8) | (imm & 0xFF))); }
static void e_b_self(void) { e16(0xE7FE); }

/* Branch offset: label_off is a link-time address, PC reads as insn+4. */
static int32_t br_off(int label)
{
    return (int32_t)label_off[label] - (int32_t)(text_len + 4);
}

static void e_b(int label)
{
    uint32_t u = (uint32_t)br_off(label);
    uint32_t s = (u >> 24) & 1, i1 = (u >> 23) & 1, i2 = (u >> 22) & 1;
    uint32_t j1 = (~(i1 ^ s)) & 1, j2 = (~(i2 ^ s)) & 1;
    e32h((uint16_t)(0xF000 | (s << 10) | ((u >> 12) & 0x3FF)),
         (uint16_t)(0x9000 | (j1 << 13) | (j2 << 11) | ((u >> 1) & 0x7FF)));
}

static void e_bl(int label)
{
    uint32_t u = (uint32_t)br_off(label);
    uint32_t s = (u >> 24) & 1, i1 = (u >> 23) & 1, i2 = (u >> 22) & 1;
    uint32_t j1 = (~(i1 ^ s)) & 1, j2 = (~(i2 ^ s)) & 1;
    e32h((uint16_t)(0xF000 | (s << 10) | ((u >> 12) & 0x3FF)),
         (uint16_t)(0xD000 | (j1 << 13) | (j2 << 11) | ((u >> 1) & 0x7FF)));
}

/* Conditional branch, cond is the ARM condition code (EQ=0, NE=1, ...). */
static void e_bcond(int cond, int label)
{
    uint32_t u = (uint32_t)br_off(label);
    uint32_t s = (u >> 24) & 1;
    uint32_t j2 = (u >> 23) & 1, j1 = (u >> 22) & 1;
    e32h((uint16_t)(0xF000 | (s << 10) | ((uint32_t)cond << 6) | ((u >> 12) & 0x3F)),
         (uint16_t)(0x8000 | (j1 << 13) | (j2 << 11) | ((u >> 1) & 0x7FF)));
}

/* Materialise a label address without relocations: the BL records the address
 * of the next instruction in LR, and the label is a link-time offset from
 * there.  LR is saved by every prologue/helper, so clobbering it is safe. */
static void e_la(int rd, int label)
{
    uint32_t here = (uint32_t)(text_len + 4);
    int32_t delta = (int32_t)label_off[label] - (int32_t)here;
    e32h(0xF000, 0xF800);            /* bl .+4  -> lr = here */
    e_bic_imm(rd, ARM_LR, 1);
    e_movw(ARM_TMP, (uint32_t)delta);
    e_movt(ARM_TMP, (uint32_t)delta);
    e_add_reg(rd, rd, ARM_TMP);
}

static int pc_label_for(int pc)
{
    int l;
    if (pc < 0 || pc >= PC_MAX)
        die("pc out of range");
    if (pc_label[pc] != PC_LABEL_NONE) {
        int lbl = (int)pc_label[pc];
        if (emit_pass == 2 && !label_seen[lbl]) {
            l = alloc_label();
            label_seen[lbl] = 1;
            if (l != lbl)
                die("label order mismatch");
        }
        return lbl;
    }
    l = alloc_label();
    if (l >= (int)PC_LABEL_NONE)
        die("too many labels");
    pc_label[pc] = (uint16_t)l;
    return l;
}

/* ---- RIR register mapping ------------------------------------------- */

static void die_unsupported(const char *what)
{
    puts("rcc: thumb2: ");
    puts(what);
    puts(" not implemented yet\n");
    exit(1);
}

static void emit_native(int op, int id, int a0, int a1, int a2, int dst);

static int vreg_ok(int v)
{
    if (v < 0 || v > 15)
        die("bad RIR register");
    return v;
}

static void ld_vreg(int tmp, int v)
{
    vreg_ok(v);
    if (v < VREG_IN_REG)
        e_mov_w(tmp, 4 + v);
    else
        e_ldr_imm(tmp, ARM_SP, 4 * (v - VREG_IN_REG));
}

static void st_vreg(int v, int src)
{
    vreg_ok(v);
    if (v < VREG_IN_REG)
        e_mov_w(4 + v, src);
    else
        e_str_imm(src, ARM_SP, 4 * (v - VREG_IN_REG));
}

/* ---- labels and per-function emission ------------------------------- */

static int L_bounds, L_pi, L_pin, L_picommon, rodata_msg;
static int L_slen, L_scmp, L_scpy, L_scat, L_atoi, L_par;
static int L_fmul, L_fdiv, L_fix, L_fixn0, L_fixn, rodata_p10;
static int rodata_empty;
static int data_bss_size;
static int func_epi[FUNC_MAX];

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

static void emit_prologue(int fid, int argc, int frame)
{
    int i;
    bind_label(func_label_for(fid));
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, frame);
    if (argc > 0) {
        int n = argc > 9 ? 9 : argc;
        e_la(ARM_SCRATCH0, data_argbuf);
        for (i = 0; i < n; i++) {
            e_ldr_imm(ARM_SCRATCH1, ARM_SCRATCH0, 4 * i);
            st_vreg(i, ARM_SCRATCH1);
        }
    }
}

static void emit_epilogue(int fid, int frame)
{
    bind_label(func_epi[fid]);
    e_add_imm(ARM_SP, ARM_SP, frame);
    e_pop();
}

/* ---- instruction selection ------------------------------------------ */

static int inv_cond(int c)
{
    switch (c) {
    case 0:  return 1;    /* EQ <-> NE  */
    case 1:  return 0;
    case 10: return 11;   /* GE <-> LT  */
    case 11: return 10;
    case 12: return 13;   /* GT <-> LE  */
    case 13: return 12;
    default: return c ^ 1;
    }
}

/* compare a,b and leave 1/0 in tmp */
static void emit_setcc(int cond, int tmp, int ra, int rb)
{
    int done = new_label();
    e_li(tmp, 0);
    e_cmp(ra, rb);
    e_bcond(inv_cond(cond), done);
    e_li(tmp, 1);
    bind_label(done);
}

static void emit_insn(int fid, int pc)
{
    int op = ir_op(pc);
    int a = ir_a(pc), b = ir_b(pc), c = ir_c(pc), d = ir_d(pc);

    switch (op) {
    case R_CONST: e_li(ARM_SCRATCH0, (uint32_t)a); st_vreg(d, ARM_SCRATCH0); break;
    case R_MOV: ld_vreg(ARM_SCRATCH0, a); st_vreg(d, ARM_SCRATCH0); break;
    case R_ADD: ld_vreg(0, a); ld_vreg(1, b); e_add_reg(2, 0, 1); st_vreg(d, 2); break;
    case R_SUB: ld_vreg(0, a); ld_vreg(1, b); e_sub_reg(2, 0, 1); st_vreg(d, 2); break;
    case R_MUL: ld_vreg(0, a); ld_vreg(1, b); e_mul(2, 0, 1); st_vreg(d, 2); break;
    case R_DIV: ld_vreg(0, a); ld_vreg(1, b); e_sdiv(2, 0, 1); st_vreg(d, 2); break;
    case R_MOD: {
        int nz = new_label(), end = new_label(), zero = new_label();
        ld_vreg(0, a);
        ld_vreg(1, b);
        e_cmp_zero(1);
        e_bcond(1 /* NE */, nz);
        e_li(2, 0);
        e_b(end);
        bind_label(nz);
        e_sdiv(2, 0, 1);
        e_mls(2, 2, 1, 0);
        bind_label(end);
        (void)zero;
        st_vreg(d, 2);
        break;
    }
    case R_LT: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(11 /* LT */, 2, 0, 1); st_vreg(d, 2); break;
    case R_GT: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(12 /* GT */, 2, 0, 1); st_vreg(d, 2); break;
    case R_EQ: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(0 /* EQ */, 2, 0, 1); st_vreg(d, 2); break;
    case R_NE: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(1 /* NE */, 2, 0, 1); st_vreg(d, 2); break;
    case R_LE: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(13 /* LE */, 2, 0, 1); st_vreg(d, 2); break;
    case R_GE: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(10 /* GE */, 2, 0, 1); st_vreg(d, 2); break;
    case R_JZ:
        ld_vreg(0, a);
        e_cmp_zero(0);
        e_bcond(0 /* EQ */, pc_label_for(b));
        break;
    case R_JMP: e_b(pc_label_for(a)); break;
    case R_RET:
        ld_vreg(0, a);
        e_b(func_epi[fid]);
        break;
    case R_HALT:
        e_movs_imm8(0, 0);
        e_svc0();
        break;
    case R_LOADG:
        e_la(ARM_SCRATCH0, data_g);
        e_ldr_imm(1, ARM_SCRATCH0, a * 4);
        st_vreg(d, 1);
        break;
    case R_STOREG:
        e_la(ARM_SCRATCH0, data_g);
        ld_vreg(1, b);
        e_str_imm(1, ARM_SCRATCH0, a * 4);
        break;
    case R_LOADA:
    case R_STOREA:
    case R_STOREA_I: {
        int lim = (op == R_LOADA) ? c : d;
        int ok = new_label();
        e_la(ARM_SCRATCH0, data_g);
        ld_vreg(1, b);
        e_li(2, (uint32_t)lim);
        e_cmp(1, 2);
        e_bcond(3 /* CC: idx < len */, ok);
        e_bl(L_bounds);
        bind_label(ok);
        if (a)
            e_add_imm(ARM_SCRATCH0, ARM_SCRATCH0, a * 4);
        e_lsl_imm(1, 1, 2);
        e_add_reg(ARM_SCRATCH0, ARM_SCRATCH0, 1);
        if (op == R_LOADA) {
            e_ldr_imm(1, ARM_SCRATCH0, 0);
            st_vreg(d, 1);
        } else if (op == R_STOREA) {
            ld_vreg(2, c);
            e_str_imm(2, ARM_SCRATCH0, 0);
        } else {
            e_li(2, (uint32_t)c);
            e_str_imm(2, ARM_SCRATCH0, 0);
        }
        break;
    }
    case R_PRINT:
        ld_vreg(0, a);
        e_bl(L_pi);
        break;
    case R_PRINTV:
        ld_vreg(0, a);
        e_bl(L_pin);
        break;
    case R_PRINTN:
    case R_PRINTS: {
        int n = b;
        if (op == R_PRINTS) {
            int off = a;
            n = 0;
            if (off >= 0 && off < strsize)
                while (off + n < strsize && strpool[off + n])
                    n++;
        }
        e_la(1, data_strpool);
        if (a) {
            e_li(2, (uint32_t)a);
            e_add_reg(1, 1, 2);
        }
        e_li(3, (uint32_t)n);   /* len */
        e_mov_w(2, 1);          /* buf */
        e_li(1, 1);             /* fd  */
        e_li(0, 1);             /* SYS_write */
        e_svc0();
        break;
    }
    case R_ARG:
        e_la(ARM_SCRATCH0, data_argbuf);
        ld_vreg(1, b);
        e_str_imm(1, ARM_SCRATCH0, a * 4);
        break;
    case R_CALL:
        e_bl(func_label_for(a));
        st_vreg(b, 0);
        break;
    case R_LOADL:
        e_ldr_imm(0, ARM_SP, 36 + 4 * a);
        st_vreg(d, 0);
        break;
    case R_STOREL:
        ld_vreg(0, a);
        e_str_imm(0, ARM_SP, 36 + 4 * b);
        break;
    case R_INC: ld_vreg(0, a); e_add_imm(2, 0, 1); st_vreg(d, 2); break;
    case R_DEC: ld_vreg(0, a); e_add_imm(2, 0, -1); st_vreg(d, 2); break;
    case R_ADDI:
        ld_vreg(0, a);
        e_li(1, (uint32_t)b);
        e_add_reg(2, 0, 1);
        st_vreg(d, 2);
        break;
    case R_SUBI:
        ld_vreg(0, a);
        e_li(1, (uint32_t)b);
        e_sub_reg(2, 0, 1);
        st_vreg(d, 2);
        break;
    case R_MULI:
        ld_vreg(0, a);
        e_li(1, (uint32_t)b);
        e_mul(2, 0, 1);
        st_vreg(d, 2);
        break;
    case R_JLT: ld_vreg(0, a); ld_vreg(1, b); e_cmp(0, 1); e_bcond(11, pc_label_for(c)); break;
    case R_JGE: ld_vreg(0, a); ld_vreg(1, b); e_cmp(0, 1); e_bcond(10, pc_label_for(c)); break;
    case R_JGT: ld_vreg(0, a); ld_vreg(1, b); e_cmp(0, 1); e_bcond(12, pc_label_for(c)); break;
    case R_JLE: ld_vreg(0, a); ld_vreg(1, b); e_cmp(0, 1); e_bcond(13, pc_label_for(c)); break;
    case R_JEQ: ld_vreg(0, a); ld_vreg(1, b); e_cmp(0, 1); e_bcond(0, pc_label_for(c)); break;
    case R_JNE: ld_vreg(0, a); ld_vreg(1, b); e_cmp(0, 1); e_bcond(1, pc_label_for(c)); break;
    case R_JLT_I:
        ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(11, pc_label_for(c)); break;
    case R_JGE_I:
        ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(10, pc_label_for(c)); break;
    case R_JGT_I:
        ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(12, pc_label_for(c)); break;
    case R_JLE_I:
        ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(13, pc_label_for(c)); break;
    case R_JEQ_I:
        ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(0, pc_label_for(c)); break;
    case R_JNE_I:
        ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(1, pc_label_for(c)); break;
    case R_NOP: e16(0xBF00); break;
    case R_SHLI:
        ld_vreg(0, a); e_lsl_imm(2, 0, b); st_vreg(d, 2); break;
    case R_SHRI:
        ld_vreg(0, a); e_asr_imm(2, 0, b); st_vreg(d, 2); break;
    case R_LOADD:
        ld_vreg(1, a);
        e_lsl_imm(1, 1, 2);
        if (b)
            e_add_imm(1, 1, b * 4);
        e_la(0, data_g);
        e_add_reg(0, 0, 1);
        e_ldr_imm(1, 0, 0);
        st_vreg(d, 1);
        break;
    case R_STORED:
        ld_vreg(1, a);
        e_lsl_imm(1, 1, 2);
        if (b)
            e_add_imm(1, 1, b * 4);
        e_la(0, data_g);
        e_add_reg(0, 0, 1);
        ld_vreg(2, c);
        e_str_imm(2, 0, 0);
        break;
    case R_LOADB:
    case R_STOREB: {
        int ok = new_label();
        ld_vreg(1, b);
        e_add_imm(1, 1, a);
        e_li(2, dsize);
        e_cmp(1, 2);
        e_bcond(3 /* CC: a+idx < dsize */, ok);
        e_bl(L_bounds);
        bind_label(ok);
        e_la(ARM_SCRATCH0, data_arena);
        e_add_reg(ARM_SCRATCH0, ARM_SCRATCH0, 1);
        if (op == R_LOADB) {
            e_ldrb_imm(2, ARM_SCRATCH0, 0);
            st_vreg(d, 2);
        } else {
            ld_vreg(2, c);
            e_strb_imm(2, ARM_SCRATCH0, 0);
        }
        break;
    }
    case R_STRLEN:
        ld_vreg(0, a);
        e_bl(L_slen);
        st_vreg(d, 0);
        break;
    case R_STRCMP:
        ld_vreg(0, a);
        ld_vreg(1, b);
        e_bl(L_scmp);
        st_vreg(d, 0);
        break;
    case R_STRCPY:
        ld_vreg(0, a);
        ld_vreg(1, b);
        ld_vreg(2, c);
        e_bl(L_scpy);
        st_vreg(d, 0);
        break;
    case R_STRCAT:
        ld_vreg(0, a);
        ld_vreg(1, b);
        ld_vreg(2, c);
        e_bl(L_scat);
        st_vreg(d, 0);
        break;
    case R_ATOI:
        ld_vreg(0, a);
        e_bl(L_atoi);
        st_vreg(d, 0);
        break;
    case R_PRINTSTR:
        ld_vreg(0, a);
        e_bl(L_par);
        break;
    case R_FMUL:
        ld_vreg(0, a);
        ld_vreg(1, b);
        e_bl(L_fmul);
        st_vreg(d, 0);
        break;
    case R_FDIV:
        ld_vreg(0, a);
        ld_vreg(1, b);
        e_bl(L_fdiv);
        st_vreg(d, 0);
        break;
    case R_PRINTFIX:
        ld_vreg(0, a);
        e_bl(L_fix);
        break;
    case R_PRINTFIXV:
        ld_vreg(0, a);
        e_bl(L_fixn0);
        break;
    case R_PRINTFIXN:
        ld_vreg(0, a);
        e_li(1, (uint32_t)b);
        e_bl(L_fixn);
        break;
    case R_NATIVE:
    case R_NATIVER:
        emit_native(op, a, b, c, op == R_NATIVE ? d : -1,
                    op == R_NATIVER ? d : -1);
        break;
    case R_BOUNDS: {
        int ok = new_label();
        ld_vreg(0, a);
        e_li(1, (uint32_t)b);
        e_cmp(0, 1);
        e_bcond(3 /* CC: index < len */, ok);
        e_bl(L_bounds);
        bind_label(ok);
        break;
    }
    default:
        {
            char b[8];
            b[0] = (char)('0' + op / 10);
            b[1] = (char)('0' + op % 10);
            b[2] = 0;
            die_unsupported(b);
        }
    }
}

static void emit_function(int fid)
{
    int pc;
    int epi = new_label();
    int frame = frame_for(fid);
    if (frame < SLOT_BYTES)
        frame = SLOT_BYTES;
    func_epi[fid] = epi;
    emit_prologue(fid, max_argc[fid], frame);
    for (pc = func_start[fid]; pc < func_end[fid]; pc++) {
        int l = pc_label_for(pc);
        if (label_off[l] < 0)
            bind_label(l);
        emit_insn(fid, pc);
    }
    emit_epilogue(fid, frame);
}

/* ---- runtime helpers ------------------------------------------------ */

static void emit_print_helper(void)
{
    /* r0 = value, r1 = newline flag; writes decimal to fd 1. */
    int pos = new_label(), digit = new_label(), sign = new_label(), nowrap = new_label();
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 32);
    e_str_imm(ARM_LR, ARM_SP, 24);
    e_str_imm(1, ARM_SP, 28);               /* newline flag */
    e_mov_w(4, 0);                          /* r4 = value (save) */
    e_mov_w(0, 4);
    e_li(6, 0);                             /* negative flag */
    e_cmp_zero(0);
    e_bcond(10 /* GE */, pos);
    e_li(6, 1);
    e_li(1, 0);
    e_sub_reg(0, 1, 0);                     /* -value */
    bind_label(pos);
    /* digits are written backwards from sp+20 */
    e_add_imm(7, ARM_SP, 20);               /* end pointer */
    e_mov_w(8, 7);                          /* cursor */
    e_li(9, 10);
    bind_label(digit);
    e_udiv(1, 0, 9);                        /* q = v / 10 */
    e_mls(2, 1, 9, 0);                      /* rem = v - q*10 */
    e_add_imm(2, 2, '0');
    e_sub_imm(8, 8, 1);
    e_strb_imm(2, 8, 0);
    e_mov_w(0, 1);
    e_cmp_zero(0);
    e_bcond(1 /* NE */, digit);
    e_cmp_zero(6);
    e_bcond(0 /* EQ */, sign);
    e_sub_imm(8, 8, 1);
    e_li(2, '-');
    e_strb_imm(2, 8, 0);
    bind_label(sign);
    e_ldr_imm(2, ARM_SP, 28);
    e_cmp_zero(2);
    e_bcond(0 /* EQ */, nowrap);
    e_li(2, '\n');
    e_strb_imm(2, 7, 0);
    e_add_imm(7, 7, 1);
    bind_label(nowrap);
    /* write(1, cursor, end-cursor) */
    e_sub_reg(3, 7, 8);                     /* len */
    e_mov_w(2, 8);                          /* buf */
    e_li(1, 1);                             /* fd  */
    e_li(0, 1);                             /* SYS_write */
    e_svc0();
    e_ldr_imm(ARM_LR, ARM_SP, 24);
    e_add_imm(ARM_SP, ARM_SP, 32);
    e_pop();
}

static int L_picommon;

/* String helpers.  The calling convention is r0/r1/r2 in, r0 out, and every
 * helper pushes r4-r11 because the caller keeps RIR values there. */
static void emit_string_helpers(void)
{
    int done, loop, ret, aok, bok, aempty, bempty, minok, diff, less, eq;
    int nok, copy, term, ret2, skip, adv, sign, plus, digits, fin, ret0;
    int srcok, srcdone, sd, nok2;

    /* ---- strlen(offset) ---------------------------------------------- */
    bind_label(L_slen);
    e_push();
    done = new_label();
    loop = new_label();
    e_la(4, data_arena);
    e_li(5, dsize);
    e_li(6, 0);
    e_cmp(0, 5);
    e_bcond(2 /* CS: unsigned off >= dsize */, done);
    e_add_reg(4, 4, 0);
    e_sub_reg(5, 5, 0);
    bind_label(loop);
    e_cmp(6, 5);
    e_bcond(2, done);
    e_ldrb_reg(7, 4, 6);
    e_cmp_zero(7);
    e_bcond(0 /* EQ */, done);
    e_add_imm(6, 6, 1);
    e_b(loop);
    bind_label(done);
    e_mov_w(0, 6);
    e_pop();

    /* ---- strcmp(a, b) ------------------------------------------------ */
    bind_label(L_scmp);
    e_push();
    aempty = new_label();
    aok = new_label();
    bempty = new_label();
    bok = new_label();
    minok = new_label();
    loop = new_label();
    diff = new_label();
    less = new_label();
    eq = new_label();
    ret = new_label();
    e_la(4, data_arena);
    e_li(5, dsize);
    e_cmp(0, 5);
    e_bcond(2, aempty);
    e_add_reg(6, 4, 0);
    e_sub_reg(7, 5, 0);
    e_b(aok);
    bind_label(aempty);
    e_la(6, rodata_empty);
    e_li(7, 1);
    bind_label(aok);
    e_cmp(1, 5);
    e_bcond(2, bempty);
    e_add_reg(8, 4, 1);
    e_sub_reg(9, 5, 1);
    e_b(bok);
    bind_label(bempty);
    e_la(8, rodata_empty);
    e_li(9, 1);
    bind_label(bok);
    e_mov_w(10, 9);
    e_cmp(7, 9);
    e_bcond(2 /* r7 >= r9 */, minok);
    e_mov_w(10, 7);
    bind_label(minok);
    e_li(11, 0);
    bind_label(loop);
    e_cmp(11, 10);
    e_bcond(2, eq);
    e_ldrb_reg(12, 6, 11);
    e_ldrb_reg(2, 8, 11);
    e_cmp(12, 2);
    e_bcond(1 /* NE */, diff);
    e_cmp_zero(12);
    e_bcond(0, eq);
    e_add_imm(11, 11, 1);
    e_b(loop);
    bind_label(diff);
    e_cmp(12, 2);
    e_bcond(3 /* CC: a < b */, less);
    e_li(0, 1);
    e_b(ret);
    bind_label(less);
    e_li(0, 0xFFFFFFFFu);
    e_b(ret);
    bind_label(eq);
    e_li(0, 0);
    bind_label(ret);
    e_pop();

    /* ---- strlcpy(dst, src, cap) -> strlen(src) -----------------------
     * Arguments live in stack slots so the two helper calls cannot clobber
     * them; only r0-r7 are used inside the copy loop. */
    bind_label(L_scpy);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 16);
    nok = new_label();
    copy = new_label();
    term = new_label();
    sd = new_label();
    ret = new_label();
    srcok = new_label();
    nok2 = new_label();
    e_str_imm(0, ARM_SP, 0);       /* dst */
    e_str_imm(1, ARM_SP, 4);       /* src */
    e_str_imm(2, ARM_SP, 8);       /* cap */
    e_ldr_imm(0, ARM_SP, 4);       /* src */
    e_bl(L_slen);
    e_str_imm(0, ARM_SP, 12);      /* srclen */
    e_ldr_imm(0, ARM_SP, 0);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(2, ret);               /* dst >= dsize */
    e_ldr_imm(2, ARM_SP, 8);
    e_cmp_zero(2);
    e_bcond(0, ret);               /* cap == 0 */
    e_sub_imm(3, 2, 1);            /* cap-1 */
    e_ldr_imm(4, ARM_SP, 12);
    e_cmp(3, 4);
    e_bcond(3, nok);               /* cap-1 < srclen */
    e_mov_w(3, 4);                 /* n = srclen */
    bind_label(nok);
    e_la(5, data_arena);
    e_ldr_imm(0, ARM_SP, 0);
    e_add_reg(5, 5, 0);            /* dstptr */
    e_ldr_imm(0, ARM_SP, 4);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(3, srcok);             /* src valid */
    e_la(6, rodata_empty);
    e_b(sd);
    bind_label(srcok);
    e_la(6, data_arena);
    e_add_reg(6, 6, 0);
    bind_label(sd);
    e_la(7, data_arena);
    e_li(1, dsize);
    e_add_reg(7, 7, 1);            /* limit */
    bind_label(term);
    e_cmp_zero(3);
    e_bcond(0, nok2);
    e_cmp(5, 7);
    e_bcond(2, nok2);
    e_ldrb_imm(0, 6, 0);
    e_strb_imm(0, 5, 0);
    e_add_imm(6, 6, 1);
    e_add_imm(5, 5, 1);
    e_sub_imm(3, 3, 1);
    e_b(term);
    bind_label(nok2);
    e_cmp(5, 7);
    e_bcond(2, ret);
    e_li(0, 0);
    e_strb_imm(0, 5, 0);
    bind_label(ret);
    e_ldr_imm(0, ARM_SP, 12);
    e_add_imm(ARM_SP, ARM_SP, 16);
    e_pop();

    /* ---- strlcat(dst, src, cap) -> strlen(dst) + strlen(src) --------- */
    bind_label(L_scat);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 16);
    nok = new_label();
    term = new_label();
    sd = new_label();
    ret = new_label();
    srcok = new_label();
    nok2 = new_label();
    e_str_imm(0, ARM_SP, 0);       /* dst */
    e_str_imm(1, ARM_SP, 4);       /* src */
    e_str_imm(2, ARM_SP, 8);       /* cap */
    e_bl(L_slen);
    e_str_imm(0, ARM_SP, 12);      /* dlen */
    e_ldr_imm(0, ARM_SP, 4);
    e_bl(L_slen);                  /* r0 = slen */
    e_mov_w(4, 0);                 /* slen */
    e_ldr_imm(5, ARM_SP, 12);      /* dlen */
    e_add_reg(6, 4, 5);            /* total */
    e_ldr_imm(0, ARM_SP, 0);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(2, ret);               /* dst >= dsize */
    e_add_imm(2, 5, 1);            /* dlen+1 */
    e_ldr_imm(3, ARM_SP, 8);
    e_cmp(2, 3);
    e_bcond(2, ret);               /* dlen+1 >= cap */
    e_sub_imm(2, 3, 1);
    e_sub_reg(2, 2, 5);            /* room */
    e_mov_w(7, 2);
    e_cmp(7, 4);
    e_bcond(3, nok);               /* room < slen */
    e_mov_w(7, 4);                 /* n = slen */
    bind_label(nok);
    e_la(3, data_arena);
    e_ldr_imm(0, ARM_SP, 0);
    e_add_reg(3, 3, 0);
    e_add_reg(8, 3, 5);            /* dstptr = arena + dst + dlen */
    e_ldr_imm(0, ARM_SP, 4);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(3, srcok);             /* src valid */
    e_la(9, rodata_empty);
    e_b(sd);
    bind_label(srcok);
    e_la(9, data_arena);
    e_add_reg(9, 9, 0);
    bind_label(sd);
    e_la(10, data_arena);
    e_li(1, dsize);
    e_add_reg(10, 10, 1);          /* limit */
    bind_label(term);
    e_cmp_zero(7);
    e_bcond(0, nok2);
    e_cmp(8, 10);
    e_bcond(2, nok2);
    e_ldrb_imm(0, 9, 0);
    e_strb_imm(0, 8, 0);
    e_add_imm(9, 9, 1);
    e_add_imm(8, 8, 1);
    e_sub_imm(7, 7, 1);
    e_b(term);
    bind_label(nok2);
    e_cmp(8, 10);
    e_bcond(2, ret);
    e_li(0, 0);
    e_strb_imm(0, 8, 0);
    bind_label(ret);
    e_mov_w(0, 6);
    e_add_imm(ARM_SP, ARM_SP, 16);
    e_pop();

    /* ---- atoi(offset) ------------------------------------------------ */
    bind_label(L_atoi);
    e_push();
    skip = new_label();
    adv = new_label();
    sign = new_label();
    plus = new_label();
    digits = new_label();
    fin = new_label();
    ret = new_label();
    ret0 = new_label();
    e_la(4, data_arena);
    e_li(5, dsize);
    e_cmp(0, 5);
    e_bcond(2, ret0);
    e_add_reg(4, 4, 0);
    e_sub_reg(5, 5, 0);
    e_li(6, 0);                    /* i   */
    e_li(7, 0);                    /* neg */
    e_li(8, 0);                    /* acc */
    bind_label(skip);
    e_cmp(6, 5);
    e_bcond(2, ret0);
    e_ldrb_reg(9, 4, 6);
    e_li(0, 32);
    e_cmp(9, 0);
    e_bcond(0, adv);
    e_li(0, 9);
    e_cmp(9, 0);
    e_bcond(0, adv);
    e_b(sign);
    bind_label(adv);
    e_add_imm(6, 6, 1);
    e_b(skip);
    bind_label(sign);
    e_li(0, '-');
    e_cmp(9, 0);
    e_bcond(1, plus);
    e_li(7, 1);
    e_add_imm(6, 6, 1);
    e_b(digits);
    bind_label(plus);
    e_li(0, '+');
    e_cmp(9, 0);
    e_bcond(1, digits);
    e_add_imm(6, 6, 1);
    bind_label(digits);
    e_cmp(6, 5);
    e_bcond(2, fin);
    e_ldrb_reg(9, 4, 6);
    e_li(0, '0');
    e_cmp(9, 0);
    e_bcond(3 /* CC: < '0' */, fin);
    e_li(0, '9');
    e_cmp(9, 0);
    e_bcond(8 /* HI: > '9' */, fin);
    e_sub_imm(9, 9, '0');
    e_li(0, 10);
    e_mul(8, 8, 0);
    e_add_reg(8, 8, 9);
    e_add_imm(6, 6, 1);
    e_b(digits);
    bind_label(fin);
    e_mov_w(0, 8);
    e_cmp_zero(7);
    e_bcond(0, ret);
    e_li(1, 0);
    e_sub_reg(0, 1, 0);
    bind_label(ret);
    e_pop();
    bind_label(ret0);
    e_li(0, 0);
    e_pop();

    /* ---- print arena string at offset -------------------------------- */
    bind_label(L_par);
    e_push();
    ret = new_label();
    loop = new_label();
    done = new_label();
    e_la(4, data_arena);
    e_li(5, dsize);
    e_cmp(0, 5);
    e_bcond(2, ret);
    e_add_reg(4, 4, 0);
    e_sub_reg(5, 5, 0);
    e_li(6, 0);
    bind_label(loop);
    e_cmp(6, 5);
    e_bcond(2, done);
    e_ldrb_reg(7, 4, 6);
    e_cmp_zero(7);
    e_bcond(0, done);
    e_add_imm(6, 6, 1);
    e_b(loop);
    bind_label(done);
    e_mov_w(2, 4);
    e_mov_w(3, 6);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    bind_label(ret);
    e_pop();
}


static void emit_native(int op, int id, int a0, int a1, int a2, int dst)
{
    int d = dst;
    switch (id) {
    case 0: /* gpio_count */
        e_li(0, 26); e_li(1, 0); e_li(2, 0); e_li(3, 0); e_svc0();
        if (d >= 0) st_vreg(d, 0);
        break;
    case 1: /* gpio_mode */
        ld_vreg(1, a0); ld_vreg(2, a1);
        e_li(0, 26); e_svc0();
        break;
    case 2: /* gpio_write */
        ld_vreg(1, a0); ld_vreg(2, a1);
        e_li(0, 26); e_svc0();
        break;
    case 3: /* gpio_read */
        ld_vreg(1, a0); e_li(2, 0);
        e_li(0, 26); e_svc0();
        if (d >= 0) st_vreg(d, 0);
        break;
    case 4: /* adc_count */
        e_li(0, 27); e_li(1, 0); e_li(2, 0); e_svc0();
        if (d >= 0) st_vreg(d, 0);
        break;
    case 5: /* adc_read */
        ld_vreg(1, a0);
        e_li(0, 27); e_svc0();
        if (d >= 0) st_vreg(d, 0);
        break;
    case 6: /* pwm_count */
        e_li(0, 40); e_li(1, 0); e_li(2, 0); e_svc0();
        if (d >= 0) st_vreg(d, 0);
        break;
    case 7: /* pwm_set: {pin, freq, duty} through data_pwmreq */
        e_la(0, data_pwmreq);
        ld_vreg(1, a0); e_str_imm(1, 0, 0);
        ld_vreg(2, a1); e_str_imm(2, 0, 4);
        ld_vreg(3, a2); e_str_imm(3, 0, 8);
        e_li(1, 1);
        e_li(0, 40);
        e_svc0();
        break;
    case 8: /* pwm_off */
        ld_vreg(2, a0); e_li(1, 2); e_li(3, 0);
        e_li(0, 40); e_svc0();
        break;
    case 9: /* sleep(ms) */
        ld_vreg(1, a0);
        e_li(0, 8); e_svc0();
        break;
    case 10: /* uptime */
        e_li(0, 9); e_svc0();
        if (d >= 0) st_vreg(d, 0);
        break;
    case 11: /* run(path) -> spawn + wait, return exit code */
        {
            int skip = new_label();
            ld_vreg(1, a0);
            e_la(0, data_arena);
            e_add_reg(1, 0, 1);
            e_li(2, 0);
            e_li(3, 0);
            e_li(0, 3);                /* SYS_EXEC */
            e_svc0();
            e_cmp_zero(0);
            e_bcond(11, skip);         /* pid < 0 */
            e_mov_w(1, 0);
            e_li(0, 12);               /* SYS_WAIT */
            e_svc0();
            bind_label(skip);
            if (d >= 0) st_vreg(d, 0);
        }
        break;
    default:
        die("bad native id");
    }
    (void)op;
}

static void emit_float_helpers(void)
{
    int loop, done, sub, nosub, clamp, posmax, nz, ret;
    int pos, dig, trim, emit, nowrap, d0, d4, dok, nocarry, nosign;
    int fixbody;

    bind_label(L_fmul);
    e_push();
    e_smull(2, 3, 0, 1);
    e_lsr_imm(2, 2, 16);
    e_lsl_imm(3, 3, 16);
    e_orr(0, 2, 3);
    e_pop();

    bind_label(L_fdiv);
    e_push();
    nz = new_label();
    clamp = new_label();
    posmax = new_label();
    loop = new_label();
    done = new_label();
    sub = new_label();
    nosub = new_label();
    ret = new_label();
    e_cmp_zero(1);
    e_bcond(1, nz);
    e_cmp_zero(0);
    e_bcond(11, clamp);
    e_li(0, 0x7FFFFFFF);
    e_b(ret);
    bind_label(nz);
    e_eor(4, 0, 1);
    e_asr_imm(4, 4, 31);
    e_asr_imm(5, 0, 31);
    e_eor(2, 0, 5);
    e_sub_reg(5, 2, 5);
    e_asr_imm(6, 1, 31);
    e_eor(2, 1, 6);
    e_sub_reg(6, 2, 6);
    e_udiv(7, 5, 6);
    e_mls(8, 7, 6, 5);
    e_li(2, 32767);
    e_cmp(7, 2);
    e_bcond(8, clamp);
    e_li(9, 0);
    e_li(10, 0);
    bind_label(loop);
    e_li(2, 16);
    e_cmp(10, 2);
    e_bcond(2, done);
    e_lsr_imm(11, 8, 31);
    e_lsl_imm(8, 8, 1);
    e_cmp_zero(11);
    e_bcond(1, sub);
    e_cmp(8, 6);
    e_bcond(3, nosub);
    bind_label(sub);
    e_sub_reg(8, 8, 6);
    e_li(2, 15);
    e_sub_reg(2, 2, 10);
    e_li(3, 1);
    e_lsl_reg(3, 3, 2);
    e_orr(9, 9, 3);
    bind_label(nosub);
    e_add_imm(10, 10, 1);
    e_b(loop);
    bind_label(done);
    e_lsl_imm(7, 7, 16);
    e_orr(0, 7, 9);
    e_eor(0, 0, 4);
    e_sub_reg(0, 0, 4);
    e_b(ret);
    bind_label(clamp);
    e_cmp_zero(4);
    e_bcond(0, posmax);
    e_li(0, 0x80000000u);
    e_b(ret);
    bind_label(posmax);
    e_li(0, 0x7FFFFFFF);
    bind_label(ret);
    e_pop();

    /* print Q16.16 with 4 decimals; L_fix adds a newline, L_fixn0 does not */
    bind_label(L_fix);
    e_li(1, 1);
    e_b(fixbody = new_label());
    bind_label(L_fixn0);
    e_li(1, 0);
    bind_label(fixbody);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 32);
    pos = new_label();
    dig = new_label();
    nowrap = new_label();
    trim = new_label();
    emit = new_label();
    e_str_imm(ARM_LR, ARM_SP, 24);
    e_str_imm(1, ARM_SP, 28);
    e_str_imm(0, ARM_SP, 0);
    e_ldr_imm(0, ARM_SP, 0);
    e_cmp_zero(0);
    e_bcond(10, pos);
    e_li(2, '-');
    e_strb_imm(2, ARM_SP, 16);
    e_li(3, 1);
    e_add_imm(2, ARM_SP, 16);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    e_ldr_imm(0, ARM_SP, 0);
    e_li(1, 0);
    e_sub_reg(0, 1, 0);
    e_str_imm(0, ARM_SP, 0);
    bind_label(pos);
    e_ldr_imm(0, ARM_SP, 0);
    e_lsr_imm(0, 0, 16);
    e_bl(L_pin);
    e_li(2, '.');
    e_strb_imm(2, ARM_SP, 16);
    e_add_imm(2, ARM_SP, 16);
    e_li(3, 1);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    e_ldr_imm(0, ARM_SP, 0);
    e_lsl_imm(0, 0, 16);
    e_lsr_imm(0, 0, 16);
    e_li(1, 10000);
    e_mul(0, 0, 1);
    e_li(1, 32768);
    e_add_reg(0, 0, 1);
    e_lsr_imm(0, 0, 16);
    e_str_imm(0, ARM_SP, 8);
    e_ldr_imm(0, ARM_SP, 8);
    e_li(1, 10);
    e_li(4, 3);
    bind_label(dig);
    e_udiv(2, 0, 1);
    e_mls(3, 2, 1, 0);
    e_add_imm(3, 3, '0');
    e_add_imm(5, ARM_SP, 12);
    e_add_reg(5, 5, 4);
    e_strb_imm(3, 5, 0);
    e_mov_w(0, 2);
    e_subs_imm(4, 4, 1);
    e_bcond(10, dig);
    e_li(4, 4);
    bind_label(trim);
    e_li(5, 1);
    e_cmp(4, 5);
    e_bcond(13 /* LE */, emit);
    e_add_imm(6, ARM_SP, 12);
    e_add_reg(6, 6, 4);
    e_sub_imm(6, 6, 1);
    e_ldrb_imm(3, 6, 0);
    e_li(7, '0');
    e_cmp(3, 7);
    e_bcond(1 /* NE */, emit);
    e_sub_imm(4, 4, 1);
    e_b(trim);
    bind_label(emit);
    e_add_imm(2, ARM_SP, 12);
    e_mov_w(3, 4);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    e_ldr_imm(2, ARM_SP, 28);
    e_cmp_zero(2);
    e_bcond(0, nowrap);
    e_li(2, '\n');
    e_strb_imm(2, ARM_SP, 16);
    e_add_imm(2, ARM_SP, 16);
    e_li(3, 1);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    bind_label(nowrap);
    e_ldr_imm(ARM_LR, ARM_SP, 24);
    e_add_imm(ARM_SP, ARM_SP, 32);
    e_pop();

    /* printf("%.Nf") */
    bind_label(L_fixn);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 48);
    d0 = new_label();
    d4 = new_label();
    dok = new_label();
    pos = new_label();
    nocarry = new_label();
    nosign = new_label();
    trim = new_label();
    emit = new_label();
    done = new_label();
    dig = new_label();
    e_str_imm(ARM_LR, ARM_SP, 44);
    e_str_imm(0, ARM_SP, 0);
    e_str_imm(1, ARM_SP, 4);
    e_ldr_imm(4, ARM_SP, 4);
    e_cmp_zero(4);
    e_bcond(11, d0);
    e_li(5, 4);
    e_cmp(4, 5);
    e_bcond(8, d4);
    e_b(dok);
    bind_label(d0);
    e_li(4, 0);
    e_b(dok);
    bind_label(d4);
    e_li(4, 4);
    bind_label(dok);
    e_str_imm(4, ARM_SP, 4);
    e_ldr_imm(0, ARM_SP, 0);
    e_li(5, 0);
    e_cmp_zero(0);
    e_bcond(10, pos);
    e_li(5, 1);
    e_li(1, 0);
    e_sub_reg(0, 1, 0);
    e_str_imm(0, ARM_SP, 0);
    bind_label(pos);
    e_str_imm(5, ARM_SP, 28);
    e_ldr_imm(0, ARM_SP, 0);
    e_lsl_imm(0, 0, 16);
    e_lsr_imm(0, 0, 16);
    e_ldr_imm(4, ARM_SP, 4);
    e_la(1, rodata_p10);
    e_lsl_imm(2, 4, 2);
    e_add_reg(1, 1, 2);
    e_ldr_imm(2, 1, 0);
    e_mul(0, 0, 2);
    e_li(3, 32768);
    e_add_reg(0, 0, 3);
    e_lsr_imm(0, 0, 16);
    e_ldr_imm(3, ARM_SP, 0);
    e_lsr_imm(3, 3, 16);
    e_cmp(0, 2);
    e_bcond(3, nocarry);
    e_sub_reg(0, 0, 2);
    e_add_imm(3, 3, 1);
    bind_label(nocarry);
    e_str_imm(3, ARM_SP, 8);
    e_str_imm(0, ARM_SP, 12);
    e_ldr_imm(5, ARM_SP, 28);
    e_cmp_zero(5);
    e_bcond(0, nosign);
    e_li(2, '-');
    e_strb_imm(2, ARM_SP, 24);
    e_add_imm(2, ARM_SP, 24);
    e_li(3, 1);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    bind_label(nosign);
    e_ldr_imm(0, ARM_SP, 8);
    e_bl(L_pin);
    e_ldr_imm(4, ARM_SP, 4);
    e_cmp_zero(4);
    e_bcond(0, done);
    e_li(2, '.');
    e_strb_imm(2, ARM_SP, 24);
    e_add_imm(2, ARM_SP, 24);
    e_li(3, 1);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    e_ldr_imm(0, ARM_SP, 12);
    e_li(1, 10);
    e_ldr_imm(5, ARM_SP, 4);
    bind_label(dig);
    e_udiv(2, 0, 1);
    e_mls(3, 2, 1, 0);
    e_add_imm(3, 3, '0');
    e_add_imm(6, ARM_SP, 16);
    e_add_reg(6, 6, 5);
    e_sub_imm(6, 6, 1);
    e_strb_imm(3, 6, 0);
    e_mov_w(0, 2);
    e_subs_imm(5, 5, 1);
    e_bcond(1, dig);
    e_li(5, 0);                /* keep exactly N digits: no trimming */
    bind_label(emit);
    e_add_imm(2, ARM_SP, 16);
    e_add_reg(2, 2, 5);
    e_ldr_imm(4, ARM_SP, 4);
    e_sub_reg(3, 4, 5);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    bind_label(done);
    e_ldr_imm(ARM_LR, ARM_SP, 44);
    e_add_imm(ARM_SP, ARM_SP, 48);
    e_pop();
}

static void emit_helpers(void)
{
    bind_label(L_bounds);
    e_la(2, rodata_msg);
    e_li(3, 7);
    e_li(1, 1);
    e_li(0, 1);                 /* SYS_write(1, "bounds\n", 7) */
    e_svc0();
    e_li(1, 1);                 /* status */
    e_li(0, 0);                 /* SYS_exit */
    e_svc0();

    bind_label(L_pi);
    e_li(1, 1);
    e_b(L_picommon);
    bind_label(L_pin);
    e_li(1, 0);
    bind_label(L_picommon);
    emit_print_helper();
    emit_string_helpers();
    emit_float_helpers();
}

static void emit_rodata(void)
{
    static const uint8_t zero = 0;
    static const uint8_t msg[] = "bounds\n";
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
    {
        static const int32_t p10[5] = { 1, 10, 100, 1000, 10000 };
        int i;
        for (i = 0; i < 5; i++)
            e32((uint32_t)p10[i]);
    }
}

void rcc_arch_emit_body(void)
{
    int i, halt;
    text_len = 0;
    halt = new_label();
    e_bl(func_label_for(main_fid));
    e_movs_imm8(0, 0);
    e_svc0();
    bind_label(halt);
    e_b_self();
    for (i = 0; i < nfunc; i++)
        emit_function(func_order[i]);
    emit_helpers();
    emit_rodata();
}

void rcc_arch_init_labels(void)
{
    L_bounds = new_label();
    L_pi = new_label();
    L_pin = new_label();
    L_picommon = new_label();
    L_slen = new_label();
    L_scmp = new_label();
    L_scpy = new_label();
    L_scat = new_label();
    L_atoi = new_label();
    L_par = new_label();
    rodata_empty = new_label();
    L_fmul = new_label();
    L_fdiv = new_label();
    L_fix = new_label();
    L_fixn0 = new_label();
    L_fixn = new_label();
    rodata_p10 = new_label();
    rodata_msg = new_label();
}

int rcc_arch_data_align(void) { return 32; }  /* MPU region size */
int rcc_arch_machine(void) { return 40; }          /* EM_ARM */
uint32_t rcc_arch_flags(void) { return 0x05000000u; } /* EABI v5 */
int rcc_arch_labels(void) { return label_next; }

#else
typedef int rcc_thumb2_backend_not_selected;
int rcc_arch_labels(void) { return 0; }
#endif
