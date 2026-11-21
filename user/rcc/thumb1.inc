/* Thumb-1 backend (ARMv6-M: Cortex-M0/M0+).  Included by rcc.c when the target
 * is an ARMv6-M core.
 *
 * Differences from thumb2.inc that shape everything here:
 *   - only the low registers (r0..r7) can be load/store bases or ALU operands;
 *     r8..r15 are reachable with mov/add/cmp only;
 *   - no IT blocks, so a "conditional long branch" is an inverted short branch
 *     over a BL;
 *   - no movw/movt, no udiv/sdiv/mls/smull.  Constants are built with
 *     movs/lsls/adds, division is a helper, and the multiplication helper is
 *     the only 32x32 the float code will need (stage 2).
 *
 * ABI: RIR 0..3 live in r4..r7 (callee saved), RIR 4..15 live in frame slots,
 * r0..r3 are scratch and r0 is the return value.  r3 is also the encoder's
 * address/scratch register, so no code may keep a value there across an
 * encoder call.  Arguments travel through data_argbuf as on the other
 * backends.
 */
#if defined(RCC_ARCH_THUMB1)

#define ARM_SCRATCH0 0
#define ARM_SCRATCH1 1
#define ARM_SCRATCH2 2
#define ARM_SCRATCH3 3
#define ARM_TMP      12
#define ARM_SP       13
#define ARM_LR       14
#define ARM_PC       15

#define VREG_IN_REG 4          /* RIR 0..3 -> r4..r7                    */
#define SLOT_BYTES 48          /* 12 slots (RIR 4..15)                  */
#define LOCAL_BASE 48          /* frame slots start here (R_LOADL base) */

/* ---- instruction encoders ------------------------------------------- */

static void e16(uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    e_bytes(b, 2);
}

static void e_need_low(const char *what, int r)
{
    if (r < 0 || r > 7) {
        puts("rcc: thumb1: ");
        puts(what);
        puts(" needs a low register\n");
        exit(1);
    }
}

/* A low scratch that is not `avoid` (r3 unless r3 is in use). */
static int lo_tmp(int avoid)
{
    return avoid == 3 ? 2 : 3;
}

static void e_add_imm(int rd, int rn, int imm);
static void e_sub_imm(int rd, int rn, int imm);

/* movs/adds/lsls build a 32-bit constant.  rd must be low. */
static void e_li(int rd, uint32_t v)
{
    e_need_low("const", rd);
    if (v < 256) {
        e16((uint16_t)(0x2000 | (rd << 8) | v));
        return;
    }
    /* start at the highest non-zero byte, then shift in the rest */
    {
        int sh = 24;
        while (((v >> sh) & 0xFF) == 0)
            sh -= 8;
        e16((uint16_t)(0x2000 | (rd << 8) | ((v >> sh) & 0xFF)));
        for (sh -= 8; sh >= 0; sh -= 8) {
            e16((uint16_t)(0x0000 | (8 << 6) | (rd << 3) | rd));   /* lsls #8 */
            e16((uint16_t)(0x3000 | (rd << 8) | ((v >> sh) & 0xFF))); /* adds */
        }
    }
}

/* mov rd, rm (high form: any registers) */
static void e_mov_w(int rd, int rm)
{
    e16((uint16_t)(0x4600 | ((rd & 8) << 4) | ((rm & 0xF) << 3) | (rd & 7)));
}

/* movs rd, #imm8 */
static void e_movs_imm8(int rd, int imm)
{
    e_need_low("movs", rd);
    e16((uint16_t)(0x2000 | (rd << 8) | (imm & 0xFF)));
}

static int pick_tmp(int a, int b)
{
    int t;
    for (t = 3; t >= 0; t--)
        if (t != a && t != b)
            return t;
    return 3;
}

/* rd = rn + rm (no flags, any registers) */
static void e_add_reg(int rd, int rn, int rm)
{
    if (rd == rn) {
        e16((uint16_t)(0x4400 | ((rd & 8) << 4) | ((rm & 0xF) << 3) | (rd & 7)));
        return;
    }
    if (rd == rm) {
        /* rd = rn + rd: keep the old rd before it is overwritten */
        int t = pick_tmp(rd, rn);
        e_mov_w(t, rm);
        e_mov_w(rd, rn);
        e16((uint16_t)(0x4400 | ((rd & 8) << 4) | ((t & 0xF) << 3) | (rd & 7)));
        return;
    }
    e_mov_w(rd, rn);
    e16((uint16_t)(0x4400 | ((rd & 8) << 4) | ((rm & 0xF) << 3) | (rd & 7)));
}

/* rd = rn - rm (sets flags; the generated code never keeps a comparison
 * across an arithmetic instruction) */
static void e_sub_reg(int rd, int rn, int rm)
{
    e_need_low("sub", rd);
    e_need_low("sub", rn);
    e_need_low("sub", rm);
    if (rd == rm && rd != rn) {
        int t = pick_tmp(rd, rn);
        e16((uint16_t)(0x0000 | (rm << 3) | t));           /* movs t, rm   */
        e16((uint16_t)(0x0000 | (rn << 3) | rd));          /* movs rd, rn  */
        e16((uint16_t)(0x1A00 | (t << 6) | (rd << 3) | rd)); /* subs rd, t */
        return;
    }
    if (rd != rn)
        e16((uint16_t)(0x0000 | (rn << 3) | rd));          /* movs rd, rn */
    e16((uint16_t)(0x1A00 | (rm << 6) | (rd << 3) | rd));  /* subs rd, rm  */
}

static void e_add_imm(int rd, int rn, int imm)
{
    if (imm < 0) {
        e_sub_imm(rd, rn, -imm);
        return;
    }
    if (rd == ARM_SP && rn == ARM_SP) {
        if (imm % 4 == 0 && imm <= 508) {
            e16((uint16_t)(0xB000 | (imm / 4)));
        } else {
            while (imm > 0) {
                int k = imm > 508 ? 508 : imm & ~3;
                e16((uint16_t)(0xB000 | (k / 4)));
                imm -= k;
            }
        }
        return;
    }
    if (rn == ARM_SP) {
        /* ADD Rd, SP, #imm8*4 */
        e_need_low("add", rd);
        if (imm % 4 == 0 && imm <= 1020) {
            e16((uint16_t)(0xA800 | (rd << 8) | (imm / 4)));
            return;
        }
    }
    e_need_low("add", rd);
    e_need_low("add", rn);
    if (rd != rn)
        e16((uint16_t)(0x0000 | (rn << 3) | rd));          /* lsls rd, rn, #0 */
    if (imm <= 255) {
        e16((uint16_t)(0x3000 | (rd << 8) | imm));         /* adds rd, #imm */
        return;
    }
    {
        int t = pick_tmp(rd, rn);       /* must not clobber rd or rn */
        e_li(t, (uint32_t)imm);
        e16((uint16_t)(0x1800 | (t << 6) | (rd << 3) | rd)); /* adds rd, t */
    }
}

static void e_sub_imm(int rd, int rn, int imm)
{
    if (imm < 0) {
        e_add_imm(rd, rn, -imm);
        return;
    }
    if (rd == ARM_SP && rn == ARM_SP) {
        if (imm % 4 == 0 && imm <= 508) {
            e16((uint16_t)(0xB080 | (imm / 4)));
        } else {
            while (imm > 0) {
                int k = imm > 508 ? 508 : imm & ~3;
                e16((uint16_t)(0xB080 | (k / 4)));
                imm -= k;
            }
        }
        return;
    }
    e_need_low("sub", rd);
    e_need_low("sub", rn);
    if (rd != rn)
        e16((uint16_t)(0x0000 | (rn << 3) | rd));
    if (imm <= 255) {
        e16((uint16_t)(0x3800 | (rd << 8) | imm));         /* subs rd, #imm */
        return;
    }
    {
        int t = pick_tmp(rd, rn);
        e_li(t, (uint32_t)imm);
        e16((uint16_t)(0x1A00 | (t << 6) | (rd << 3) | rd)); /* subs rd, t */
    }
}

static void e_subs_imm(int rd, int rn, int imm)
{
    e_sub_imm(rd, rn, imm);
}

static void e_mul(int rd, int rn, int rm)
{
    e_need_low("mul", rd);
    e_need_low("mul", rn);
    e_need_low("mul", rm);
    if (rd == rm && rd != rn) {
        int t = pick_tmp(rd, rn);
        e16((uint16_t)(0x0000 | (rm << 3) | t));           /* movs t, rm  */
        e16((uint16_t)(0x0000 | (rn << 3) | rd));          /* movs rd, rn */
        e16((uint16_t)(0x4340 | (t << 3) | rd));           /* muls rd, t  */
        return;
    }
    if (rd != rn)
        e16((uint16_t)(0x0000 | (rn << 3) | rd));
    e16((uint16_t)(0x4340 | (rm << 3) | rd));              /* muls rd, rm  */
}

/* rd = ra - rn*rm.  The product lives in a scratch that must differ from rd
 * (it is overwritten with ra), rm (still needed for the multiply) and ra (its
 * value is copied after the multiply).  Callers keep r0-r3 free apart from
 * the operands, so the scratch always comes from that set. */
static void e_mls(int rd, int rn, int rm, int ra)
{
    int t;
    e_need_low("mls", rd);
    e_need_low("mls", rn);
    e_need_low("mls", rm);
    e_need_low("mls", ra);
    for (t = 3; t >= 0; t--)
        if (t != rd && t != rm && t != ra)
            break;
    e16((uint16_t)(0x0000 | (rn << 3) | t));               /* movs t, rn  */
    e16((uint16_t)(0x4340 | (rm << 3) | t));               /* muls t, rm  */
    if (rd == ra) {
        e16((uint16_t)(0x1A00 | (t << 6) | (rd << 3) | rd)); /* subs rd, t */
        return;
    }
    e_mov_w(rd, ra);
    e16((uint16_t)(0x1A00 | (t << 6) | (rd << 3) | rd));
}

/* Division and the 32x32 low half are emitted helpers (see the helper
 * section); the encoders only marshal the arguments. */
static void e_sdiv(int rd, int rn, int rm);
static void e_udiv(int rd, int rn, int rm);

static void e_cmp(int rn, int rm)
{
    e16((uint16_t)(0x4500 | ((rn & 8) << 4) | ((rm & 0xF) << 3) | (rn & 7)));
}

static void e_cmp_zero(int rn)
{
    if (rn < 8) {
        e16((uint16_t)(0x2800 | (rn << 8)));
        return;
    }
    /* cmp rn, r3 after zeroing r3 (r3 is the encoder scratch) */
    e16((uint16_t)(0x2300));                               /* movs r3, #0  */
    e_cmp(rn, 3);
}

/* Thumb-1 shifts are three-operand but low only. */
static void e_shift_imm(int rd, int rm, int sh, int type);

static void e_lsl_imm(int rd, int rm, int sh) { e_shift_imm(rd, rm, sh, 0); }
static void e_lsr_imm(int rd, int rm, int sh) { e_shift_imm(rd, rm, sh, 1); }
static void e_asr_imm(int rd, int rm, int sh) { e_shift_imm(rd, rm, sh, 2); }

static void e_shift_imm(int rd, int rm, int sh, int type)
{
    uint16_t base = type == 0 ? 0x0000 : type == 1 ? 0x0800 : 0x1000;
    e_need_low("shift", rd);
    e_need_low("shift", rm);
    if (sh == 0 && type == 0) {
        if (rd != rm)
            e16((uint16_t)(0x0000 | (rm << 3) | rd));
        return;
    }
    if (sh < 0 || sh > 31) {
        /* 32 or more: lsr/asr give 0 or the sign, lsl gives 0 */
        if (type == 0) {
            e16((uint16_t)(0x2300 | ((rd & 7) << 8)));     /* movs rd, #0 */
            return;
        }
        sh = 31;
    }
    e16((uint16_t)(base | (sh << 6) | (rm << 3) | rd));
}

static void e_ldr_imm(int rd, int rn, int off)
{
    e_need_low("ldr", rd);
    if (rn == ARM_SP) {
        if (off % 4 == 0 && off >= 0 && off <= 1020) {
            e16((uint16_t)(0x9800 | (rd << 8) | (off / 4)));
            return;
        }
        die("thumb1: sp offset out of range");
    }
    if (rn < 8 && off >= 0 && off % 4 == 0 && off <= 124) {
        e16((uint16_t)(0x6800 | ((off / 4) << 6) | (rn << 3) | rd));
        return;
    }
    {
        int t = lo_tmp(rd);
        e_add_imm(t, rn, off);                  /* t = rn + off */
        e16((uint16_t)(0x6800 | (t << 3) | rd));       /* ldr rd, [t]   */
    }
}

static void e_str_imm(int rd, int rn, int off)
{
    e_need_low("str", rd);
    if (rn == ARM_SP) {
        if (off % 4 == 0 && off >= 0 && off <= 1020) {
            e16((uint16_t)(0x9000 | (rd << 8) | (off / 4)));
            return;
        }
        die("thumb1: sp offset out of range");
    }
    if (rn < 8 && off >= 0 && off % 4 == 0 && off <= 124) {
        e16((uint16_t)(0x6000 | ((off / 4) << 6) | (rn << 3) | rd));
        return;
    }
    {
        int t = lo_tmp(rd);
        e_add_imm(t, rn, off);
        e16((uint16_t)(0x6000 | (t << 3) | rd));
    }
}

static void e_ldrb_imm(int rd, int rn, int off)
{
    e_need_low("ldrb", rd);
    if (rn < 8 && off >= 0 && off <= 31) {
        e16((uint16_t)(0x7800 | ((off & 31) << 6) | (rn << 3) | rd));
        return;
    }
    {
        int t = lo_tmp(rd);
        e_add_imm(t, rn, off);
        e16((uint16_t)(0x7800 | (t << 3) | rd));
    }
}

static void e_strb_imm(int rd, int rn, int off)
{
    e_need_low("strb", rd);
    if (rn < 8 && off >= 0 && off <= 31) {
        e16((uint16_t)(0x7000 | ((off & 31) << 6) | (rn << 3) | rd));
        return;
    }
    {
        int t = lo_tmp(rd);
        e_add_imm(t, rn, off);
        e16((uint16_t)(0x7000 | (t << 3) | rd));
    }
}

static void e_ldrb_reg(int rd, int rn, int rm)
{
    e_need_low("ldrb", rd);
    e_need_low("ldrb", rn);
    e_need_low("ldrb", rm);
    e16((uint16_t)(0x5C00 | (rm << 6) | (rn << 3) | rd));
}

static void e_strb_reg(int rd, int rn, int rm)
{
    e_need_low("strb", rd);
    e_need_low("strb", rn);
    e_need_low("strb", rm);
    e16((uint16_t)(0x5400 | (rm << 6) | (rn << 3) | rd));
}

static void e_ldr_reg_shift(int rd, int rn, int rm, int sh)
{
    e_need_low("ldr", rd);
    e_need_low("ldr", rn);
    e_need_low("ldr", rm);
    if (sh) {
        e16((uint16_t)(0x0000 | (sh << 6) | (rm << 3) | rm));  /* lsls rm, #sh */
    }
    e16((uint16_t)(0x5800 | (rm << 6) | (rn << 3) | rd));
}

static void e_str_reg_shift(int rd, int rn, int rm, int sh)
{
    e_need_low("str", rd);
    e_need_low("str", rn);
    e_need_low("str", rm);
    if (sh) {
        e16((uint16_t)(0x0000 | (sh << 6) | (rm << 3) | rm));
    }
    e16((uint16_t)(0x5000 | (rm << 6) | (rn << 3) | rd));
}

/* Two-operand data processing: rd = rd OP rm. */
static void e_dp(int op, int rd, int rn, int rm)
{
    e_need_low("alu", rd);
    e_need_low("alu", rn);
    e_need_low("alu", rm);
    if (rd != rn)
        e16((uint16_t)(0x0000 | (rn << 3) | rd));
    e16((uint16_t)(0x4000 | (op << 6) | (rm << 3) | rd));
}

static void e_and(int rd, int rn, int rm) { e_dp(0, rd, rn, rm); }
static void e_eor(int rd, int rn, int rm) { e_dp(1, rd, rn, rm); }
static void e_lsl_reg(int rd, int rn, int rm) { e_dp(2, rd, rn, rm); }
static void e_lsr_reg(int rd, int rn, int rm) { e_dp(3, rd, rn, rm); }
static void e_asr_reg(int rd, int rn, int rm) { e_dp(4, rd, rn, rm); }
static void e_orr(int rd, int rn, int rm) { e_dp(12, rd, rn, rm); }

static void e_bic_imm(int rd, int rn, int imm)
{
    int t = lo_tmp(rd);
    e_li(t, (uint32_t)imm);
    e_dp(14, rd, rn, t);
}

static void e_push(void)  { e16(0xB5F0); }   /* {r4-r7, lr} */
static void e_pop(void)   { e16(0xBDF0); }   /* {r4-r7, pc} */
static void e_svc0(void)  { e16(0xDF00); }
static void e_b_self(void) { e16(0xE7FE); }

/* ---- branches -------------------------------------------------------- */

static int inv_cond(int c);

/* Branch offset: label_off is a link-time address, PC reads as insn+4. */
static int32_t br_off(int label)
{
    return (int32_t)label_off[label] - (int32_t)(text_len + 4);
}

/* Unconditional transfer: BL has a 16 MB range and writes LR, which every
 * prologue and helper saves, so a jump may share its encoding. */
static void e_bl(int label)
{
    uint32_t u = (uint32_t)br_off(label);
    uint32_t s = (u >> 24) & 1, i1 = (u >> 23) & 1, i2 = (u >> 22) & 1;
    uint32_t j1 = (~(i1 ^ s)) & 1, j2 = (~(i2 ^ s)) & 1;
    e16((uint16_t)(0xF000 | (s << 10) | ((u >> 12) & 0x3FF)));
    e16((uint16_t)(0xD000 | (j1 << 13) | (j2 << 11) | ((u >> 1) & 0x7FF)));
}

static void e_b(int label)
{
    e_bl(label);
}

/* A real two-byte branch.  Only for labels that are already bound (backward
 * jumps): the encoding cannot be chosen from a distance that pass 1 does not
 * know yet without changing the code size between passes. */
static void e_bb(int label)
{
    int32_t d;
    if (label_off[label] < 0)
        die("thumb1: forward short branch");
    d = (int32_t)label_off[label] - (int32_t)(text_len + 4);
    if ((d & 1) || d < -2048 || d > 2046)
        die("thumb1: short branch out of range");
    e16((uint16_t)(0xE000 | ((d >> 1) & 0x7FF)));
}

/* Conditional transfer: skip the BL when the condition is false.  Thumb-1 has
 * no conditional BL, and a short conditional branch cannot reach across a
 * large function, so the pair is always "b<inv> +4 ; bl target". */
static void e_bcond(int cond, int label)
{
    e16((uint16_t)(0xD000 | (inv_cond(cond) << 8) | 1));
    e_bl(label);
}

/* Materialise a label address.  `mov rd, pc` gives this instruction + 4 with
 * bit 0 clear; text offsets are always even, so adding a signed delta keeps
 * the Thumb bit clear.  No relocation table is needed.  The delta is only
 * known for sure in pass 2, so the constant builder here always emits seven
 * instructions: a size that depends on the value would break the pass-1/pass-2
 * comparison this compiler relies on.
 *
 * The scratch is saved: helpers keep live values in r2/r3 (r3 is also the
 * encoder's general scratch), and e_la must not destroy them. */
static void e_li32(int rd, uint32_t v)
{
    e16((uint16_t)(0x2000 | (rd << 8) | ((v >> 24) & 0xFF)));
    e16((uint16_t)(0x0000 | (8 << 6) | (rd << 3) | rd));
    e16((uint16_t)(0x3000 | (rd << 8) | ((v >> 16) & 0xFF)));
    e16((uint16_t)(0x0000 | (8 << 6) | (rd << 3) | rd));
    e16((uint16_t)(0x3000 | (rd << 8) | ((v >> 8) & 0xFF)));
    e16((uint16_t)(0x0000 | (8 << 6) | (rd << 3) | rd));
    e16((uint16_t)(0x3000 | (rd << 8) | (v & 0xFF)));
}

static void e_la(int rd, int label)
{
    int t = (rd == 3) ? 2 : 3;
    uint32_t here;
    int32_t delta;

    e16((uint16_t)(0xB400 | (1 << t)));      /* push {rt} */
    here = (uint32_t)(text_len + 4);
    delta = (int32_t)label_off[label] - (int32_t)here;
    e_mov_w(rd, ARM_PC);
    e_li32(t, (uint32_t)delta);
    e_add_reg(rd, rd, t);                    /* high form: any rd */
    e16((uint16_t)(0xBC00 | (1 << t)));      /* pop {rt}  */
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
    puts("rcc: thumb1: ");
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
static int L_udiv, L_sdiv, L_smull;
static int L_fmul, L_fdiv, L_fix, L_fixn0, L_fixn, L_capture;
static int rodata_empty, rodata_p10;
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
        int n = argc > 12 ? 12 : argc;
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

static int inv_cond(int c);
static void emit_setcc(int cond, int tmp, int ra, int rb)
{
    int done = new_label();
    e_li(tmp, 0);
    e_cmp(ra, rb);
    e_bcond(inv_cond(cond), done);
    e_li(tmp, 1);
    bind_label(done);
}

static int inv_cond(int c)
{
    switch (c) {
    case 0:  return 1;
    case 1:  return 0;
    case 10: return 11;
    case 11: return 10;
    case 12: return 13;
    case 13: return 12;
    default: return c ^ 1;
    }
}

/* Signed/unsigned division marshal r0/r1 and call the helpers. */
static void e_udiv(int rd, int rn, int rm)
{
    e_need_low("udiv", rd);
    if (rn != 0 && rm != 1) {
        e_mov_w(0, rn);
        e_mov_w(1, rm);
    } else if (rn == 1 && rm == 0) {
        e_mov_w(2, rn);
        e_mov_w(1, rm);
        e_mov_w(0, 2);
    } else {
        if (rn != 0)
            e_mov_w(0, rn);
        if (rm != 1)
            e_mov_w(1, rm);
    }
    e_bl(L_udiv);
    if (rd != 0)
        e_mov_w(rd, 0);
}

static void e_sdiv(int rd, int rn, int rm)
{
    e_need_low("sdiv", rd);
    if (rn != 0)
        e_mov_w(0, rn);
    if (rm != 1)
        e_mov_w(1, rm);
    e_bl(L_sdiv);
    if (rd != 0)
        e_mov_w(rd, 0);
}

static void emit_native_argbuf(int id, int dst)
{
    switch (id) {
    case 18: /* capture(path, buf, cap) */
        e_la(0, data_argbuf);
        e_ldr_imm(1, 0, 0);
        e_ldr_imm(2, 0, 4);
        e_ldr_imm(3, 0, 8);
        e_la(0, data_arena);
        e_add_reg(1, 0, 1);
        e_add_reg(2, 0, 2);
        e_mov_w(0, 1);
        e_mov_w(1, 2);
        e_mov_w(2, 3);
        e_bl(L_capture);
        break;
    case 19: /* readdir(index, buf, len) */
        e_la(0, data_argbuf);
        e_ldr_imm(1, 0, 0);
        e_ldr_imm(2, 0, 4);
        e_ldr_imm(3, 0, 8);
        e_la(0, data_arena);
        e_add_reg(2, 0, 2);
        e_li(0, 6);
        e_svc0();
        break;
    case 20: { /* isdir(path) */
        int no = new_label(), yes = new_label(), done = new_label();
        e_la(0, data_argbuf);
        e_ldr_imm(1, 0, 0);
        e_la(0, data_arena);
        e_add_reg(1, 0, 1);
        e_la(2, data_statbuf);
        e_li(0, 18);
        e_svc0();
        e_cmp_zero(0);
        e_bcond(11, no);
        e_la(2, data_statbuf);
        e_ldrb_imm(3, 2, 4);
        e_li(1, 1);
        e_cmp(3, 1);
        e_bcond(0, yes);
        e_li(0, 0);
        e_b(done);
        bind_label(no);
        e_li(0, 0xFFFFFFFFu);
        e_b(done);
        bind_label(yes);
        e_li(0, 1);
        bind_label(done);
        break;
    }
    case 12: /* open(path) */
    case 13: /* create(path) */
    case 17: /* unlink(path) */
        e_la(0, data_argbuf);
        e_ldr_imm(1, 0, 0);
        e_la(0, data_arena);
        e_add_reg(1, 0, 1);
        e_li(0, id == 12 ? 4 : id == 13 ? 11 : 31);
        e_svc0();
        break;
    case 14: /* read(fd, buf, len) */
    case 15: /* write(fd, buf, len) */
        e_la(0, data_argbuf);
        e_ldr_imm(1, 0, 0);
        e_ldr_imm(2, 0, 4);
        e_ldr_imm(3, 0, 8);
        e_la(0, data_arena);
        e_add_reg(2, 0, 2);
        e_li(0, id == 14 ? 2 : 1);
        e_svc0();
        break;
    case 16: /* close(fd) */
        e_la(0, data_argbuf);
        e_ldr_imm(1, 0, 0);
        e_li(0, 5);
        e_svc0();
        break;
    default:
        die("bad argbuf native");
    }
    if (dst >= 0)
        st_vreg(dst, 0);
}

static void emit_insn(int fid, int pc)
{
    int op = ir_op(pc);
    int a = ir_a(pc), b = ir_b(pc), c = ir_c(pc), d = ir_d(pc);

    switch (op) {
    case R_CONST:
        e_li(0, (uint32_t)a);
        st_vreg(d, 0);
        break;
    case R_MOV:
        ld_vreg(0, a);
        st_vreg(d, 0);
        break;
    case R_ADD: ld_vreg(0, a); ld_vreg(1, b); e_add_reg(2, 0, 1); st_vreg(d, 2); break;
    case R_SUB: ld_vreg(0, a); ld_vreg(1, b); e_sub_reg(2, 0, 1); st_vreg(d, 2); break;
    case R_MUL: ld_vreg(0, a); ld_vreg(1, b); e_mul(2, 0, 1); st_vreg(d, 2); break;
    case R_DIV: ld_vreg(0, a); ld_vreg(1, b); e_sdiv(2, 0, 1); st_vreg(d, 2); break;
    case R_MOD: {
        int nz = new_label(), end = new_label();
        ld_vreg(0, a);
        ld_vreg(1, b);
        e_cmp_zero(1);
        e_bcond(1 /* NE */, nz);
        e_li(2, 0);
        e_b(end);
        bind_label(nz);
        e_sdiv(2, 0, 1);
        ld_vreg(0, a);          /* the divide helper clobbers r0 */
        e_mls(2, 2, 1, 0);
        bind_label(end);
        st_vreg(d, 2);
        break;
    }
    case R_LT: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(11, 2, 0, 1); st_vreg(d, 2); break;
    case R_GT: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(12, 2, 0, 1); st_vreg(d, 2); break;
    case R_EQ: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(0, 2, 0, 1); st_vreg(d, 2); break;
    case R_NE: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(1, 2, 0, 1); st_vreg(d, 2); break;
    case R_LE: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(13, 2, 0, 1); st_vreg(d, 2); break;
    case R_GE: ld_vreg(0, a); ld_vreg(1, b); emit_setcc(10, 2, 0, 1); st_vreg(d, 2); break;
    case R_JZ:
        ld_vreg(0, a);
        e_cmp_zero(0);
        e_bcond(0, pc_label_for(b));
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
        e_la(0, data_g);
        e_ldr_imm(1, 0, a * 4);
        st_vreg(d, 1);
        break;
    case R_STOREG:
        e_la(0, data_g);
        ld_vreg(1, b);
        e_str_imm(1, 0, a * 4);
        break;
    case R_LOADA:
    case R_STOREA:
    case R_STOREA_I: {
        int lim = (op == R_LOADA) ? c : d;
        int ok = new_label();
        e_la(0, data_g);
        ld_vreg(1, b);
        e_li(2, (uint32_t)lim);
        e_cmp(1, 2);
        e_bcond(3 /* CC */, ok);
        e_bl(L_bounds);
        bind_label(ok);
        if (a)
            e_add_imm(0, 0, a * 4);
        e_lsl_imm(1, 1, 2);
        e_add_reg(0, 0, 1);
        if (op == R_LOADA) {
            e_ldr_imm(1, 0, 0);
            st_vreg(d, 1);
        } else if (op == R_STOREA) {
            ld_vreg(2, c);
            e_str_imm(2, 0, 0);
        } else {
            e_li(2, (uint32_t)c);
            e_str_imm(2, 0, 0);
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
        e_li(3, (uint32_t)n);
        e_mov_w(2, 1);
        e_li(1, 1);
        e_li(0, 1);
        e_svc0();
        break;
    }
    case R_ARG:
        e_la(0, data_argbuf);
        ld_vreg(1, b);
        e_str_imm(1, 0, a * 4);
        break;
    case R_CALL:
        e_bl(func_label_for(a));
        st_vreg(b, 0);
        break;
    case R_LOADL:
        e_ldr_imm(0, ARM_SP, LOCAL_BASE + 4 * a);
        st_vreg(d, 0);
        break;
    case R_STOREL:
        ld_vreg(0, a);
        e_str_imm(0, ARM_SP, LOCAL_BASE + 4 * b);
        break;
    case R_INC: ld_vreg(0, a); e_add_imm(2, 0, 1); st_vreg(d, 2); break;
    case R_DEC: ld_vreg(0, a); e_sub_imm(2, 0, 1); st_vreg(d, 2); break;
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
    case R_JLT_I: ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(11, pc_label_for(c)); break;
    case R_JGE_I: ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(10, pc_label_for(c)); break;
    case R_JGT_I: ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(12, pc_label_for(c)); break;
    case R_JLE_I: ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(13, pc_label_for(c)); break;
    case R_JEQ_I: ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(0, pc_label_for(c)); break;
    case R_JNE_I: ld_vreg(0, a); e_li(1, (uint32_t)b); e_cmp(0, 1); e_bcond(1, pc_label_for(c)); break;
    case R_NOP: e16(0xBF00); break;
    case R_SHLI: ld_vreg(0, a); e_lsl_imm(2, 0, b); st_vreg(d, 2); break;
    case R_SHRI: ld_vreg(0, a); e_asr_imm(2, 0, b); st_vreg(d, 2); break;
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
        e_bcond(3, ok);
        e_bl(L_bounds);
        bind_label(ok);
        e_la(0, data_arena);
        e_add_reg(0, 0, 1);
        if (op == R_LOADB) {
            e_ldrb_imm(2, 0, 0);
            st_vreg(d, 2);
        } else {
            ld_vreg(2, c);
            e_strb_imm(2, 0, 0);
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
    case R_NATIVEC:
        emit_native_argbuf(a, b);
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
        e_bcond(3, ok);
        e_bl(L_bounds);
        bind_label(ok);
        break;
    }
    default:
        {
            char s[8];
            s[0] = (char)('0' + op / 10);
            s[1] = (char)('0' + op % 10);
            s[2] = 0;
            die_unsupported(s);
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

static void e_rsbs(int rd, int rn)
{
    e_need_low("rsbs", rd);
    e_need_low("rsbs", rn);
    e16((uint16_t)(0x4240 | (rn << 3) | rd));      /* rsbs rd, rn, #0 */
}

static void e_adc(int rd, int rm)
{
    e_need_low("adc", rd);
    e_need_low("adc", rm);
    e16((uint16_t)(0x4140 | (rm << 3) | rd));      /* adcs rd, rm     */
}

static void e_adds_reg(int rd, int rm)
{
    e_need_low("adds", rd);
    e_need_low("adds", rm);
    e16((uint16_t)(0x1800 | (rm << 6) | (rd << 3) | rd));
}

/* unsigned divide: r0 / r1 -> r0; clobbers r1-r3, preserves r4-r7 */
static void emit_udiv_helper(void)
{
    int loop = new_label(), nosub = new_label(), next = new_label();
    int zero = new_label(), out = new_label();

    bind_label(L_udiv);
    e_push();
    e_cmp_zero(1);
    e_bcond(0 /* EQ */, zero);
    e_movs_imm8(4, 32);                 /* counter  */
    e_mov_w(5, 0);                      /* dividend */
    e_li(2, 0);                         /* quotient */
    e_li(3, 0);                         /* remainder */
    bind_label(loop);
    e_lsl_imm(5, 5, 1);                 /* C = next bit */
    e_adc(3, 3);                        /* rem = rem*2 + C */
    e_cmp(3, 1);
    e_bcond(3 /* CC */, nosub);
    e_sub_reg(3, 3, 1);
    e_lsl_imm(2, 2, 1);
    e_add_imm(2, 2, 1);
    e_b(next);
    bind_label(nosub);
    e_lsl_imm(2, 2, 1);
    bind_label(next);
    e_sub_imm(4, 4, 1);
    e_bcond(1 /* NE */, loop);
    e_mov_w(0, 2);
    e_b(out);
    bind_label(zero);
    e_li(0, 0);
    bind_label(out);
    e_pop();
}

/* signed divide: r0 / r1 -> r0, truncating */
static void emit_sdiv_helper(void)
{
    int aok = new_label(), bok = new_label(), out = new_label();

    bind_label(L_sdiv);
    e_push();
    e_li(6, 0);                         /* sign */
    e_cmp_zero(0);
    e_bcond(10 /* GE */, aok);
    e_rsbs(0, 0);
    e_add_imm(6, 6, 1);
    bind_label(aok);
    e_cmp_zero(1);
    e_bcond(10, bok);
    e_rsbs(1, 1);
    e_add_imm(6, 6, 1);
    bind_label(bok);
    e_bl(L_udiv);
    e_cmp_zero(6);
    e_bcond(0 /* EQ */, out);
    e_rsbs(0, 0);
    bind_label(out);
    e_pop();
}

/* r0 = value, r1 = newline flag; writes decimal to fd 1.
 * Frame: [sp+0..15] digits, [sp+16] optional newline, [sp+20] flag.  The
 * digits and the newline address stay inside the 32 bytes the helper owns;
 * the old layout wrote the newline at [sp+16], which is the caller's saved
 * r4 once push has run. */
static void emit_print_helper(void)
{
    int digit = new_label(), pos = new_label(), sign = new_label();
    int nowrap = new_label();

    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 32);
    e_str_imm(1, ARM_SP, 20);           /* newline flag */
    e_mov_w(4, 0);                      /* value */
    e_li(5, 0);                         /* negative */
    e_cmp_zero(4);
    e_bcond(10 /* GE */, pos);
    e_li(5, 1);
    e_li(0, 0);
    e_sub_reg(4, 0, 4);
    bind_label(pos);
    e_add_imm(7, ARM_SP, 16);           /* end of the digit buffer */
    e_mov_w(6, 7);                      /* cursor, moves down      */
    bind_label(digit);
    e_mov_w(0, 4);
    e_li(1, 10);
    e_bl(L_udiv);                       /* r0 = value / 10 */
    e_li(2, 10);
    e_mul(3, 0, 2);                     /* r3 = q * 10     */
    e_sub_reg(2, 4, 3);                 /* r2 = remainder  */
    e_add_imm(2, 2, '0');
    e_sub_imm(6, 6, 1);
    e_strb_imm(2, 6, 0);
    e_mov_w(4, 0);
    e_cmp_zero(4);
    e_bcond(1 /* NE */, digit);
    e_cmp_zero(5);
    e_bcond(0 /* EQ */, sign);
    e_sub_imm(6, 6, 1);
    e_li(2, '-');
    e_strb_imm(2, 6, 0);
    bind_label(sign);
    e_ldr_imm(2, ARM_SP, 20);
    e_cmp_zero(2);
    e_bcond(0 /* EQ */, nowrap);
    e_li(2, '\n');
    e_strb_imm(2, 7, 0);                /* [sp+16], inside the frame */
    e_add_imm(7, 7, 1);
    bind_label(nowrap);
    e_sub_reg(3, 7, 6);                 /* length */
    e_mov_w(2, 6);                      /* buffer */
    e_li(1, 1);                         /* fd     */
    e_li(0, 1);                         /* SYS_write */
    e_svc0();
    e_add_imm(ARM_SP, ARM_SP, 32);
    e_pop();
}

/* ---- strings -------------------------------------------------------- */

/* strlen(off) -> length, bounded by dsize */
static void emit_slen_helper(void)
{
    int loop = new_label(), done = new_label(), zero = new_label();

    bind_label(L_slen);
    e_push();
    e_la(1, data_arena);
    e_li(2, dsize);
    e_cmp(0, 2);
    e_bcond(2 /* CS */, zero);
    e_add_reg(1, 1, 0);                 /* ptr */
    e_sub_reg(2, 2, 0);                 /* limit */
    e_li(4, 0);
    bind_label(loop);
    e_cmp(4, 2);
    e_bcond(2, done);
    e_ldrb_reg(3, 1, 4);
    e_cmp_zero(3);
    e_bcond(0, done);
    e_add_imm(4, 4, 1);
    e_b(loop);
    bind_label(done);
    e_mov_w(0, 4);
    e_b(zero);
    bind_label(zero);
    e_pop();
}

/* strcmp(a, b) -> -1/0/1 */
static void emit_scmp_helper(void)
{
    int loop = new_label(), eq = new_label(), diff = new_label();
    int less = new_label(), ret = new_label(), aempty = new_label();
    int bempty = new_label();

    bind_label(L_scmp);
    e_push();
    e_la(2, data_arena);
    e_li(3, dsize);
    e_cmp(0, 3);
    e_bcond(2, aempty);
    e_cmp(1, 3);
    e_bcond(2, bempty);
    e_mov_w(6, 0);                      /* ia */
    e_mov_w(7, 1);                      /* ib */
    bind_label(loop);
    e_cmp(6, 3);
    e_bcond(2, eq);                     /* ia >= dsize     */
    e_cmp(7, 3);
    e_bcond(2, eq);
    e_ldrb_reg(0, 2, 6);
    e_ldrb_reg(1, 2, 7);
    e_cmp(0, 1);
    e_bcond(1, diff);
    e_cmp_zero(0);
    e_bcond(0, eq);
    e_add_imm(6, 6, 1);
    e_add_imm(7, 7, 1);
    e_b(loop);
    bind_label(diff);
    e_bcond(3 /* CC: a < b */, less);
    e_li(0, 1);
    e_b(ret);
    bind_label(less);
    e_li(0, 0xFFFFFFFFu);
    e_b(ret);
    bind_label(aempty);
    e_cmp(1, 3);
    e_bcond(2, eq);                     /* both empty */
    e_li(0, 0xFFFFFFFFu);
    e_b(ret);
    bind_label(bempty);
    e_li(0, 1);
    e_b(ret);
    bind_label(eq);
    e_li(0, 0);
    bind_label(ret);
    e_pop();
}

/* strlcpy(dst, src, cap) -> strlen(src); arguments live in frame slots
 * because both helper calls clobber r0-r3. */
static void emit_scpy_helper(void)
{
    int ret = new_label(), srcok = new_label(), nok = new_label();
    int loop = new_label(), term = new_label(), dstok = new_label();

    bind_label(L_scpy);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 16);
    e_str_imm(0, ARM_SP, 0);            /* dst */
    e_str_imm(1, ARM_SP, 4);            /* src */
    e_str_imm(2, ARM_SP, 8);            /* cap */
    e_ldr_imm(0, ARM_SP, 4);
    e_bl(L_slen);
    e_str_imm(0, ARM_SP, 12);           /* srclen */
    e_ldr_imm(0, ARM_SP, 0);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(2 /* CS */, ret);
    e_ldr_imm(2, ARM_SP, 8);
    e_cmp_zero(2);
    e_bcond(0, ret);
    e_sub_imm(3, 2, 1);                 /* cap-1 */
    e_ldr_imm(4, ARM_SP, 12);
    e_cmp(3, 4);
    e_bcond(3 /* CC */, nok);
    e_mov_w(3, 4);                      /* n = srclen */
    bind_label(nok);
    e_la(5, data_arena);
    e_ldr_imm(0, ARM_SP, 0);
    e_add_reg(5, 5, 0);                 /* dstptr */
    e_ldr_imm(0, ARM_SP, 4);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(3, srcok);
    e_la(6, rodata_empty);
    e_b(dstok);
    bind_label(srcok);
    e_la(6, data_arena);
    e_ldr_imm(0, ARM_SP, 4);
    e_add_reg(6, 6, 0);
    bind_label(dstok);
    e_la(7, data_arena);
    e_li(1, dsize);
    e_add_reg(7, 7, 1);                 /* limit */
    bind_label(loop);
    e_cmp_zero(3);
    e_bcond(0, term);
    e_cmp(5, 7);
    e_bcond(2, term);
    e_ldrb_imm(0, 6, 0);
    e_strb_imm(0, 5, 0);
    e_add_imm(6, 6, 1);
    e_add_imm(5, 5, 1);
    e_sub_imm(3, 3, 1);
    e_b(loop);
    bind_label(term);
    e_cmp(5, 7);
    e_bcond(2, ret);
    e_li(0, 0);
    e_strb_imm(0, 5, 0);
    bind_label(ret);
    e_ldr_imm(0, ARM_SP, 12);
    e_add_imm(ARM_SP, ARM_SP, 16);
    e_pop();
}

/* strlcat(dst, src, cap) -> strlen(dst) + strlen(src) */
static void emit_scat_helper(void)
{
    int ret = new_label(), nok = new_label(), srcok = new_label();
    int loop = new_label(), term = new_label(), dstok = new_label();

    bind_label(L_scat);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 16);
    e_str_imm(0, ARM_SP, 0);            /* dst */
    e_str_imm(1, ARM_SP, 4);            /* src */
    e_str_imm(2, ARM_SP, 8);            /* cap */
    e_ldr_imm(0, ARM_SP, 0);
    e_bl(L_slen);
    e_str_imm(0, ARM_SP, 12);           /* dlen */
    e_ldr_imm(0, ARM_SP, 4);
    e_bl(L_slen);
    e_mov_w(4, 0);                      /* slen */
    e_ldr_imm(5, ARM_SP, 12);           /* dlen */
    e_add_reg(6, 4, 5);                 /* total */
    e_ldr_imm(0, ARM_SP, 0);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(2, ret);
    e_add_imm(2, 5, 1);                 /* dlen+1 */
    e_ldr_imm(3, ARM_SP, 8);
    e_cmp(2, 3);
    e_bcond(2, ret);                    /* dlen+1 >= cap */
    e_sub_imm(2, 3, 1);
    e_sub_reg(2, 2, 5);                 /* room */
    e_mov_w(7, 2);
    e_cmp(7, 4);
    e_bcond(3, nok);                    /* room < slen */
    e_mov_w(7, 4);                      /* n = slen */
    bind_label(nok);
    e_la(3, data_arena);
    e_ldr_imm(0, ARM_SP, 0);
    e_add_reg(3, 3, 0);
    e_add_reg(3, 3, 5);                 /* dstptr = arena+dst+dlen */
    e_ldr_imm(0, ARM_SP, 4);
    e_li(1, dsize);
    e_cmp(0, 1);
    e_bcond(3, srcok);
    e_la(2, rodata_empty);
    e_b(dstok);
    bind_label(srcok);
    e_la(2, data_arena);
    e_ldr_imm(0, ARM_SP, 4);
    e_add_reg(2, 2, 0);
    bind_label(dstok);
    bind_label(loop);
    e_cmp_zero(7);
    e_bcond(0, term);
    e_ldrb_imm(0, 2, 0);
    e_strb_imm(0, 3, 0);
    e_add_imm(2, 2, 1);
    e_add_imm(3, 3, 1);
    e_sub_imm(7, 7, 1);
    e_b(loop);
    bind_label(term);
    e_la(0, data_arena);
    e_li(1, dsize);
    e_add_reg(0, 0, 1);
    e_cmp(3, 0);
    e_bcond(2, ret);
    e_li(0, 0);
    e_strb_imm(0, 3, 0);
    bind_label(ret);
    e_mov_w(0, 6);
    e_add_imm(ARM_SP, ARM_SP, 16);
    e_pop();
}

/* atoi(off) -> int */
static void emit_atoi_helper(void)
{
    int ws = new_label(), sign = new_label(), plus = new_label();
    int digits = new_label(), fin = new_label(), out = new_label();
    int zero = new_label();

    bind_label(L_atoi);
    e_push();
    e_li(2, dsize);
    e_cmp(0, 2);
    e_bcond(2, zero);
    e_la(1, data_arena);
    e_add_reg(1, 1, 0);                 /* ptr */
    e_sub_reg(2, 2, 0);                 /* remaining */
    e_li(3, 0);                         /* value */
    e_li(4, 0);                         /* negative */
    bind_label(ws);
    e_cmp_zero(2);
    e_bcond(0, fin);
    e_ldrb_imm(5, 1, 0);
    e_li(6, ' ');
    e_cmp(5, 6);
    e_bcond(1 /* NE */, sign);
    e_add_imm(1, 1, 1);
    e_sub_imm(2, 2, 1);
    e_b(ws);
    bind_label(sign);
    e_li(6, '-');
    e_cmp(5, 6);
    e_bcond(1, plus);
    e_li(4, 1);
    e_add_imm(1, 1, 1);
    e_sub_imm(2, 2, 1);
    e_b(digits);
    bind_label(plus);
    e_li(6, '+');
    e_cmp(5, 6);
    e_bcond(1, digits);
    e_add_imm(1, 1, 1);
    e_sub_imm(2, 2, 1);
    bind_label(digits);
    e_cmp_zero(2);
    e_bcond(0, fin);
    e_ldrb_imm(5, 1, 0);
    e_li(6, '0');
    e_cmp(5, 6);
    e_bcond(3, fin);                    /* < '0' */
    e_li(6, '9');
    e_cmp(5, 6);
    e_bcond(8, fin);                    /* > '9' */
    e_sub_imm(5, 5, '0');
    e_li(6, 10);
    e_mul(3, 3, 6);
    e_add_reg(3, 3, 5);
    e_add_imm(1, 1, 1);
    e_sub_imm(2, 2, 1);
    e_b(digits);
    bind_label(fin);
    e_cmp_zero(4);
    e_bcond(0, out);
    e_rsbs(3, 3);
    bind_label(out);
    e_mov_w(0, 3);
    e_b(zero);
    bind_label(zero);
    e_pop();
}

/* print the arena string at offset */
static void emit_par_helper(void)
{
    int loop = new_label(), done = new_label(), ret = new_label();

    bind_label(L_par);
    e_push();
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

/* ---- float (Q16.16) -------------------------------------------------- */

/* smull: r0 * r1 -> r0 = low half, r1 = high half (signed).
 * 16x16 partial products; r7 is the carry scratch. */
static void emit_smull_helper(void)
{
    bind_label(L_smull);
    e_push();
    e_asr_imm(2, 0, 16);                /* ah */
    e_li(6, 0xFFFF);
    e_and(3, 0, 6);                     /* al */
    e_asr_imm(4, 1, 16);                /* bh */
    e_and(5, 1, 6);                     /* bl */
    e_mul(0, 3, 5);                     /* lo = al*bl        */
    e_mul(1, 3, 4);                     /* al*bh             */
    e_mul(7, 2, 5);                     /* ah*bl             */
    e_adds_reg(1, 7);                   /* cross low, C=carry */
    e_mul(6, 2, 4);                     /* hi = ah*bh        */
    e_movs_imm8(7, 0);
    e_adc(6, 7);                        /* hi += carry       */
    e_mov_w(7, 1);
    e_lsr_imm(7, 7, 16);
    e_adds_reg(6, 7);                   /* hi += cross >> 16 */
    e_lsl_imm(1, 1, 16);
    e_adds_reg(0, 1);                   /* lo += cross << 16 */
    e_movs_imm8(7, 0);
    e_adc(6, 7);                        /* hi += carry       */
    e_mov_w(1, 6);
    e_pop();
}

/* Q16.16 multiply: r0 = (a*b) >> 16 */
static void emit_fmul_helper(void)
{
    bind_label(L_fmul);
    e_push();
    e_bl(L_smull);                      /* r0 = lo, r1 = hi */
    e_lsr_imm(2, 0, 16);
    e_lsl_imm(3, 1, 16);
    e_orr(0, 2, 3);
    e_pop();
}

/* Q16.16 divide: r0 = (a << 16) / b, clamped to the int32 range */
static void emit_fdiv_helper(void)
{
    int nz = new_label(), clamp = new_label(), posmax = new_label();
    int loop = new_label(), sub = new_label(), nosub = new_label();
    int out = new_label();

    bind_label(L_fdiv);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 8);
    e_cmp_zero(1);
    e_bcond(1 /* NE */, nz);
    e_cmp_zero(0);
    e_bcond(11 /* LT */, clamp);
    e_li(0, 0x7FFFFFFF);
    e_b(out);
    bind_label(nz);
    e_eor(2, 0, 1);
    e_asr_imm(2, 2, 31);
    e_str_imm(2, ARM_SP, 0);            /* sign */
    e_asr_imm(3, 0, 31);
    e_eor(4, 0, 3);
    e_sub_reg(4, 4, 3);                 /* |a| */
    e_asr_imm(3, 1, 31);
    e_eor(5, 1, 3);
    e_sub_reg(5, 5, 3);                 /* |b| */
    e_udiv(6, 4, 5);
    e_mls(7, 6, 5, 4);                  /* rem */
    e_li(0, 32767);
    e_cmp(6, 0);
    e_bcond(8 /* HI */, clamp);
    e_li(0, 0);                         /* frac */
    e_li(1, 16);                        /* i    */
    bind_label(loop);
    e_lsr_imm(2, 7, 31);
    e_lsl_imm(7, 7, 1);
    e_cmp_zero(2);
    e_bcond(1, sub);
    e_cmp(7, 5);
    e_bcond(3 /* CC */, nosub);
    bind_label(sub);
    e_sub_reg(7, 7, 5);
    e_sub_imm(2, 1, 1);                 /* shift = counter - 1 (15..0) */
    e_li(3, 1);
    e_lsl_reg(3, 3, 2);
    e_orr(0, 0, 3);
    bind_label(nosub);
    e_sub_imm(1, 1, 1);
    e_bcond(1 /* NE */, loop);
    e_lsl_imm(6, 6, 16);
    e_orr(0, 0, 6);
    e_ldr_imm(2, ARM_SP, 0);
    e_cmp_zero(2);
    e_bcond(0 /* EQ */, out);
    e_rsbs(0, 0);
    e_b(out);
    bind_label(clamp);
    e_ldr_imm(2, ARM_SP, 0);
    e_cmp_zero(2);
    e_bcond(0, posmax);
    e_li(0, 0x80000000u);
    e_b(out);
    bind_label(posmax);
    e_li(0, 0x7FFFFFFF);
    bind_label(out);
    e_add_imm(ARM_SP, ARM_SP, 8);
    e_pop();
}

/* print Q16.16 with up to four decimals.  L_fix adds a newline and falls into
 * the body; L_fixn0 sits after it and branches back (a BL there would clobber
 * the return address the body's push saves). */
static void emit_fix_helper(void)
{
    int body = new_label(), pos = new_label(), dig = new_label();
    int trim = new_label(), emit = new_label(), nowrap = new_label();

    bind_label(L_fix);
    e_li(1, 1);
    bind_label(body);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 32);
    e_str_imm(1, ARM_SP, 28);
    e_str_imm(0, ARM_SP, 0);
    e_cmp_zero(0);
    e_bcond(10 /* GE */, pos);
    e_li(2, '-');
    e_strb_imm(2, ARM_SP, 16);
    e_add_imm(2, ARM_SP, 16);
    e_li(3, 1);
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
    e_mov_w(6, 0);                      /* the divider clobbers r0 */
    e_udiv(2, 0, 1);
    e_mul(3, 2, 1);                     /* q*10 */
    e_mov_w(0, 2);                      /* next value */
    e_sub_reg(3, 6, 3);                 /* remainder */
    e_add_imm(3, 3, '0');
    e_add_imm(5, ARM_SP, 12);
    e_add_reg(5, 5, 4);
    e_strb_imm(3, 5, 0);
    e_subs_imm(4, 4, 1);
    e_bcond(10 /* GE */, dig);
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
    e_add_imm(ARM_SP, ARM_SP, 32);
    e_pop();

    bind_label(L_fixn0);
    e_li(1, 0);
    e_bb(body);
}

/* printf("%.Nf"): r0 = value, r1 = decimals (clamped to 0..4) */
static void emit_fixn_helper(void)
{
    int d0 = new_label(), d4 = new_label(), dok = new_label();
    int pos = new_label(), nocarry = new_label(), nosign = new_label();
    int dig = new_label(), emit = new_label(), done = new_label();

    bind_label(L_fixn);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 48);
    e_str_imm(0, ARM_SP, 0);            /* value */
    e_str_imm(1, ARM_SP, 4);            /* decimals */
    e_ldr_imm(4, ARM_SP, 4);
    e_cmp_zero(4);
    e_bcond(11 /* LT */, d0);
    e_li(5, 4);
    e_cmp(4, 5);
    e_bcond(8 /* HI */, d4);
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
    e_ldr_imm(2, 1, 0);                 /* scale = p10[prec] */
    e_mul(0, 0, 2);                     /* frac * scale      */
    e_li(3, 32768);
    e_add_reg(0, 0, 3);
    e_lsr_imm(0, 0, 16);
    e_ldr_imm(3, ARM_SP, 0);
    e_lsr_imm(3, 3, 16);                /* integer part */
    e_cmp(0, 2);
    e_bcond(3 /* CC */, nocarry);
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
    e_mov_w(6, 0);
    e_udiv(2, 0, 1);
    e_mul(3, 2, 1);
    e_mov_w(0, 2);
    e_sub_reg(3, 6, 3);
    e_add_imm(3, 3, '0');
    e_add_imm(6, ARM_SP, 16);
    e_add_reg(6, 6, 5);
    e_sub_imm(6, 6, 1);
    e_strb_imm(3, 6, 0);
    e_subs_imm(5, 5, 1);
    e_bcond(1 /* NE */, dig);
    e_li(5, 0);
    bind_label(emit);
    e_add_imm(2, ARM_SP, 16);
    e_add_reg(2, 2, 5);
    e_ldr_imm(4, ARM_SP, 4);
    e_sub_reg(3, 4, 5);
    e_li(1, 1);
    e_li(0, 1);
    e_svc0();
    bind_label(done);
    e_add_imm(ARM_SP, ARM_SP, 48);
    e_pop();
}

/* capture(path, buf, cap): both pipe ends are moved above fd 2 first. */
static void emit_capture_helper(void)
{
    int loop = new_label(), done = new_label(), childfail = new_label();
    int fail = new_label(), ret = new_label(), f0ok = new_label();
    int f1ok = new_label();

    bind_label(L_capture);
    e_push();
    e_sub_imm(ARM_SP, ARM_SP, 32);
    e_str_imm(0, ARM_SP, 0);            /* path */
    e_str_imm(1, ARM_SP, 4);            /* buf  */
    e_str_imm(2, ARM_SP, 8);            /* cap  */
    e_add_imm(1, ARM_SP, 16);
    e_li(0, 28); e_svc0();              /* pipe */
    e_cmp_zero(0);
    e_bcond(11, fail);
    e_ldr_imm(2, ARM_SP, 16);
    e_cmp(2, 3);
    e_bcond(10 /* GE */, f0ok);
    e_mov_w(1, 2); e_li(0, 35); e_svc0();   /* dup */
    e_str_imm(0, ARM_SP, 16);
    e_mov_w(1, 2); e_li(0, 5); e_svc0();
    bind_label(f0ok);
    e_ldr_imm(2, ARM_SP, 20);
    e_cmp(2, 3);
    e_bcond(10, f1ok);
    e_mov_w(1, 2); e_li(0, 35); e_svc0();
    e_str_imm(0, ARM_SP, 20);
    e_mov_w(1, 2); e_li(0, 5); e_svc0();
    bind_label(f1ok);
    e_ldr_imm(1, ARM_SP, 20);
    e_li(2, 1);
    e_li(0, 29); e_svc0();              /* dup2(write, 1) */
    e_cmp_zero(0);
    e_bcond(11, fail);
    e_ldr_imm(1, ARM_SP, 20);
    e_li(0, 5); e_svc0();
    e_ldr_imm(1, ARM_SP, 0);
    e_li(2, 0);
    e_li(3, 0);
    e_li(0, 3); e_svc0();               /* exec */
    e_str_imm(0, ARM_SP, 12);
    e_li(1, 1); e_li(0, 5); e_svc0();   /* close(1) */
    e_ldr_imm(0, ARM_SP, 12);
    e_cmp_zero(0);
    e_bcond(11, childfail);
    e_li(4, 0);
    bind_label(loop);
    e_ldr_imm(5, ARM_SP, 8);
    e_cmp(4, 5);
    e_bcond(10, done);
    e_ldr_imm(1, ARM_SP, 16);
    e_ldr_imm(2, ARM_SP, 4);
    e_add_reg(2, 2, 4);
    e_ldr_imm(3, ARM_SP, 8);
    e_sub_reg(3, 3, 4);
    e_li(0, 2); e_svc0();
    e_cmp_zero(0);
    e_bcond(0, done);
    e_bcond(11, done);
    e_add_reg(4, 4, 0);
    e_b(loop);
    bind_label(done);
    e_ldr_imm(1, ARM_SP, 16);
    e_li(0, 5); e_svc0();
    e_ldr_imm(1, ARM_SP, 12);
    e_li(0, 12); e_svc0();              /* wait */
    e_mov_w(0, 4);
    e_b(ret);
    bind_label(childfail);
    e_ldr_imm(1, ARM_SP, 16);
    e_li(0, 5); e_svc0();
    e_li(0, 0xFFFFFFF2u);
    e_b(ret);
    bind_label(fail);
    e_li(0, 0xFFFFFFFFu);
    bind_label(ret);
    e_add_imm(ARM_SP, ARM_SP, 32);
    e_pop();
}

static void emit_helpers(void)
{
    bind_label(L_bounds);
    e_la(2, rodata_msg);
    e_li(3, 7);
    e_li(1, 1);
    e_li(0, 1);                         /* SYS_write(1, "bounds\n", 7) */
    e_svc0();
    e_li(1, 1);                         /* status */
    e_li(0, 0);                         /* SYS_exit */
    e_svc0();

    /* L_pi falls into the body; L_pin sits after it and branches back.  A BL
     * (the long-branch encoding) would clobber the return address that the
     * helper's push saves, so this one jump has to be a real short branch. */
    bind_label(L_pi);
    e_li(1, 1);
    bind_label(L_picommon);
    emit_print_helper();
    bind_label(L_pin);
    e_li(1, 0);
    e_bb(L_picommon);
    emit_udiv_helper();
    emit_sdiv_helper();
    emit_slen_helper();
    emit_scmp_helper();
    emit_scpy_helper();
    emit_scat_helper();
    emit_atoi_helper();
    emit_par_helper();
    emit_smull_helper();
    emit_fmul_helper();
    emit_fdiv_helper();
    emit_fix_helper();
    emit_fixn_helper();
    emit_capture_helper();
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
    case 7: /* pwm_set */
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
    case 11: { /* run(path) */
        int skip = new_label();
        ld_vreg(1, a0);
        e_la(0, data_arena);
        e_add_reg(1, 0, 1);
        e_li(2, 0);
        e_li(3, 0);
        e_li(0, 3);
        e_svc0();
        e_cmp_zero(0);
        e_bcond(11 /* LT */, skip);
        e_mov_w(1, 0);
        e_li(0, 12);
        e_svc0();
        bind_label(skip);
        if (d >= 0) st_vreg(d, 0);
        break;
    }
    default:
        die("bad native id");
    }
    (void)op;
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
    L_udiv = new_label();
    L_sdiv = new_label();
    L_smull = new_label();
    L_fmul = new_label();
    L_fdiv = new_label();
    L_fix = new_label();
    L_fixn0 = new_label();
    L_fixn = new_label();
    L_capture = new_label();
    rodata_empty = new_label();
    rodata_p10 = new_label();
    rodata_msg = new_label();
}

int rcc_arch_data_align(void) { return 32; }   /* MPU region size */
int rcc_arch_machine(void) { return 40; }      /* EM_ARM */
uint32_t rcc_arch_flags(void) { return 0x05000000u; } /* EABI v5 */
int rcc_arch_labels(void) { return label_next; }

#else
typedef int rcc_thumb1_backend_not_selected;
int rcc_arch_labels(void) { return 0; }
#endif
