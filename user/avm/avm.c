#include "avm.h"
#include <string.h>

/* Core instruction set (Pawn 4.1).  Values match the AMX enum; overlay,
 * packed and most macro opcodes are intentionally absent. */
enum {
    OP_NOP = 0, OP_LOAD_PRI, OP_LOAD_ALT, OP_LOAD_S_PRI, OP_LOAD_S_ALT,
    OP_LREF_S_PRI, OP_LREF_S_ALT, OP_LOAD_I, OP_LODB_I, OP_CONST_PRI,
    OP_CONST_ALT, OP_ADDR_PRI, OP_ADDR_ALT, OP_STOR, OP_STOR_S, OP_SREF_S,
    OP_STOR_I, OP_STRB_I, OP_ALIGN_PRI, OP_LCTRL, OP_SCTRL, OP_XCHG,
    OP_PUSH_PRI, OP_PUSH_ALT, OP_PUSHR_PRI, OP_POP_PRI, OP_POP_ALT,
    OP_PICK, OP_STACK, OP_HEAP, OP_PROC, OP_RET, OP_RETN, OP_CALL,
    OP_JUMP, OP_JZER, OP_JNZ, OP_SHL, OP_SHR, OP_SSHR, OP_SHL_C_PRI,
    OP_SHL_C_ALT, OP_SMUL, OP_SDIV, OP_ADD, OP_SUB, OP_AND, OP_OR,
    OP_XOR, OP_NOT, OP_NEG, OP_INVERT, OP_EQ, OP_NEQ, OP_SLESS, OP_SLEQ,
    OP_SGRTR, OP_SGEQ, OP_INC_PRI, OP_INC_ALT, OP_INC_I, OP_DEC_PRI,
    OP_DEC_ALT, OP_DEC_I, OP_MOVS, OP_CMPS, OP_FILL, OP_HALT, OP_BOUNDS,
    OP_SYSREQ, OP_SWITCH, OP_SWAP_PRI, OP_SWAP_ALT, OP_BREAK, OP_CASETBL,
    OP_SYSREQ_D, OP_SYSREQ_ND, OP_CALL_OVL, OP_RETN_OVL, OP_SWITCH_OVL,
    OP_CASETBL_OVL,
    OP_LIDX, OP_LIDX_B, OP_IDXADDR, OP_IDXADDR_B, OP_PUSH_C, OP_PUSH,
    OP_PUSH_S, OP_PUSH_ADR, OP_PUSHR_C, OP_PUSHR_S, OP_PUSHR_ADR,
    OP_SYSREQ_N
};

typedef struct {
    int32_t size;
    uint16_t magic;
    uint8_t file_version, amx_version;
    int16_t flags, defsize;
    int32_t cod, dat, hea, stp, cip;
    int32_t publics, natives, libraries, pubvars, tags, nametable, overlays;
} __attribute__((packed)) avm_header;

typedef struct {
    uint32_t address;
    uint32_t nameofs;
} __attribute__((packed)) avm_funcstub;

void *avm_address(avm *vm, acell a)
{
#if UINTPTR_MAX > 0xffffffffu
    return (void *)(((uintptr_t)vm->data & ~(uintptr_t)0xffffffffu) |
                    (uint32_t)a);
#else
    (void)vm;
    return (void *)(uintptr_t)(uint32_t)a;
#endif
}

int avm_getstring(char *dst, const acell *src, int cap)
{
    int n = 0;

    if ((uint32_t)*src > 0x00ffffffu) {
        const acell *s = src;
        int idx = 3;
        acell c = 0;
        for (;;) {
            if (idx == 3)
                c = *s++;
            int ch = (int)((c >> (idx * 8)) & 0xff);
            if (ch == 0)
                break;
            if (n < cap - 1)
                dst[n++] = (char)ch;
            idx = (idx + 3) & 3;
        }
    } else {
        while (*src != 0 && n < cap - 1)
            dst[n++] = (char)(*src++);
    }
    dst[n] = '\0';
    return n;
}

static avm_native_fn find_native(const avm_native_info *list, const char *name)
{
    if (!list)
        return NULL;
    for (int i = 0; list[i].name; i++)
        if (strcmp(list[i].name, name) == 0)
            return list[i].fn;
    return NULL;
}

int avm_init(avm *vm, void *block, uint32_t size,
             const avm_native_info *natives)
{
    avm_header *h = (avm_header *)block;

    memset(vm, 0, sizeof(*vm));
    vm->base = (uint8_t *)block;
    if (size < sizeof(avm_header) || h->magic != 0xf1e0)
        return AVM_ERR_FORMAT;
    if (h->defsize != 8 || h->cod < (int32_t)sizeof(avm_header) ||
        h->dat < h->cod || h->hea < h->dat || h->stp <= h->hea ||
        (uint32_t)h->size > size)
        return AVM_ERR_FORMAT;
    if (h->flags & 0x01)   /* AMX_FLAG_OVERLAY: not supported */
        return AVM_ERR_FORMAT;
    if ((uint32_t)h->hea > size)
        return AVM_ERR_FORMAT;

    vm->code = vm->base + h->cod;
    vm->data = vm->base + h->dat;
    vm->code_size = h->dat - h->cod;
    vm->hlw = h->hea - h->dat;
    vm->stp = h->stp - h->dat - 4;
    vm->hea = vm->hlw;
    vm->stk = vm->stp;
    vm->cip = h->cip;
    vm->frm = 0;
    vm->pri = vm->alt = 0;

    /* argument count, then the return address 0 (AMX_EXEC_MAIN) */
    vm->stk -= 4;
    *(acell *)(vm->data + vm->stk) = 0;
    vm->stk -= 4;
    *(acell *)(vm->data + vm->stk) = 0;

    if (natives && h->libraries >= h->natives && h->defsize > 0) {
        int count = (h->libraries - h->natives) / h->defsize;
        for (int i = 0; i < count; i++) {
            avm_funcstub *fs = (avm_funcstub *)
                (vm->base + h->natives + i * h->defsize);
            const char *name = (const char *)(vm->base + fs->nameofs);
            avm_native_fn fn = find_native(natives, name);
            if (!fn) {
                vm->missing = name;
                return AVM_ERR_NOTFOUND;
            }
            if (i >= AVM_MAX_NATIVES)
                return AVM_ERR_FORMAT;
            vm->natives[i] = fn;
        }
    }
    return AVM_OK;
}

/* Hot loop state lives in locals, exactly like the reference interpreter.
 * vm->pri/alt/cip/stk/hea/frm are only written on return or before a native
 * call, so the compiler can keep the loop in registers. */
#define R(off)      (*(acell *)(data + (off)))
#define W(off, v)   (*(acell *)(data + (off)) = (v))
#define C(at)       (*(acell *)(code + (at)))
#define FETCH()     (*(acell *)(code + (cip += 4) - 4))
#define PUSH(v)     do { acell _x = (v); stk -= 4; W(stk, _x); } while (0)
#define POP(v)      do { (v) = R(stk); stk += 4; } while (0)
#define JUMPREL(at) ((at) + C(at) - 4)
#define ADDR_OK(off) \
    ((uint32_t)(off) < (uint32_t)stp && \
     !((uint32_t)(off) >= (uint32_t)hea && (uint32_t)(off) < (uint32_t)stk))
#define MARGIN_OK() ((uint32_t)hea + 64u <= (uint32_t)stk)
#define SYNC() \
    do { vm->pri = pri; vm->alt = alt; vm->cip = cip; vm->stk = stk; \
         vm->hea = hea; vm->frm = frm; } while (0)
#define FAIL(e) do { vm->error = (e); SYNC(); return (e); } while (0)

int avm_run(avm *vm, uint32_t budget, acell *retval)
{
    acell pri = vm->pri, alt = vm->alt;
    int32_t cip = vm->cip, stk = vm->stk, hea = vm->hea, frm = vm->frm;
    const int32_t stp = vm->stp, hlw = vm->hlw;
    const int32_t code_size = vm->code_size;
    uint8_t *data = vm->data, *code = vm->code;

    vm->error = 0;
    for (;;) {
        if (budget == 0) {
            SYNC();
            return AVM_BUDGET;
        }
        if (budget != 0xffffffffu)
            budget--;
        if (cip < 0 || (uint32_t)cip + 4 > (uint32_t)code_size)
            FAIL(AVM_ERR_MEM);

        acell op = FETCH();
        switch (op) {
        case OP_NOP:
            break;
        case OP_LOAD_PRI:
            pri = R(FETCH());
            break;
        case OP_LOAD_ALT:
            alt = R(FETCH());
            break;
        case OP_LOAD_S_PRI:
            pri = R(frm + FETCH());
            break;
        case OP_LOAD_S_ALT:
            alt = R(frm + FETCH());
            break;
        case OP_LREF_S_PRI: {
            int32_t o = R(frm + FETCH());
            pri = R(o);
            break;
        }
        case OP_LREF_S_ALT: {
            int32_t o = R(frm + FETCH());
            alt = R(o);
            break;
        }
        case OP_LOAD_I:
            if (!ADDR_OK(pri))
                FAIL(AVM_ERR_MEM);
            pri = R(pri);
            break;
        case OP_LODB_I: {
            int32_t width = FETCH();
            if (!ADDR_OK(pri))
                FAIL(AVM_ERR_MEM);
            if (width == 1)
                pri = *(uint8_t *)(data + pri);
            else if (width == 2)
                pri = *(uint16_t *)(data + pri);
            else
                pri = R(pri);
            break;
        }
        case OP_CONST_PRI:
            pri = FETCH();
            break;
        case OP_CONST_ALT:
            alt = FETCH();
            break;
        case OP_ADDR_PRI:
            pri = FETCH() + frm;
            break;
        case OP_ADDR_ALT:
            alt = FETCH() + frm;
            break;
        case OP_STOR:
            W(FETCH(), pri);
            break;
        case OP_STOR_S:
            W(frm + FETCH(), pri);
            break;
        case OP_SREF_S: {
            int32_t o = R(frm + FETCH());
            W(o, pri);
            break;
        }
        case OP_STOR_I:
            if (!ADDR_OK(alt))
                FAIL(AVM_ERR_MEM);
            W(alt, pri);
            break;
        case OP_STRB_I: {
            int32_t width = FETCH();
            if (!ADDR_OK(alt))
                FAIL(AVM_ERR_MEM);
            if (width == 1)
                *(uint8_t *)(data + alt) = (uint8_t)pri;
            else if (width == 2)
                *(uint16_t *)(data + alt) = (uint16_t)pri;
            else
                W(alt, pri);
            break;
        }
        case OP_ALIGN_PRI: {
            int32_t o = FETCH();
            if (o < 4)
                pri ^= 4 - o;
            break;
        }
        case OP_LCTRL: {
            int32_t o = FETCH();
            switch (o) {
            case 0: pri = (int32_t)(code - vm->base); break;
            case 1: pri = (int32_t)(data - vm->base); break;
            case 2: pri = hea; break;
            case 3: pri = stp; break;
            case 4: pri = stk; break;
            case 5: pri = frm; break;
            case 6: pri = cip; break;
            }
            break;
        }
        case OP_SCTRL: {
            int32_t o = FETCH();
            if (o == 2) hea = pri;
            else if (o == 4) stk = pri;
            else if (o == 5) frm = pri;
            else if (o == 6) cip = pri;
            break;
        }
        case OP_XCHG: {
            acell t = pri;
            pri = alt;
            alt = t;
            break;
        }
        case OP_PUSH_PRI:
            PUSH(pri);
            break;
        case OP_PUSH_ALT:
            PUSH(alt);
            break;
        case OP_PUSHR_PRI:
            PUSH((acell)(uintptr_t)(data + (int)pri));
            break;
        case OP_POP_PRI:
            POP(pri);
            break;
        case OP_POP_ALT:
            POP(alt);
            break;
        case OP_PICK:
            pri = R(stk + FETCH());
            break;
        case OP_STACK:
            stk += FETCH();
            alt = stk;
            if (!MARGIN_OK() || stk > stp)
                FAIL(AVM_ERR_STACK);
            break;
        case OP_HEAP:
            alt = hea;
            hea += FETCH();
            if (!MARGIN_OK() || hea < hlw)
                FAIL(AVM_ERR_STACK);
            break;
        case OP_PROC:
            PUSH(frm);
            frm = stk;
            if (!MARGIN_OK())
                FAIL(AVM_ERR_STACK);
            break;
        case OP_RET:
            POP(frm);
            POP(cip);
            break;
        case OP_RETN:
            POP(frm);
            POP(cip);
            stk += R(stk) + 4;
            break;
        case OP_CALL: {
            int32_t ret = cip + 4;
            int32_t target = JUMPREL(cip);
            PUSH(ret);
            cip = target;
            break;
        }
        case OP_JUMP:
            cip = JUMPREL(cip);
            break;
        case OP_JZER: {
            int32_t target = JUMPREL(cip);
            if (pri == 0) cip = target;
            else cip += 4;
            break;
        }
        case OP_JNZ: {
            int32_t target = JUMPREL(cip);
            if (pri != 0) cip = target;
            else cip += 4;
            break;
        }
        case OP_SHL:
            pri = (acell)((uint32_t)pri << (alt & 31));
            break;
        case OP_SHR:
            pri = (acell)((uint32_t)pri >> (alt & 31));
            break;
        case OP_SSHR:
            pri = pri >> (alt & 31);
            break;
        case OP_SHL_C_PRI:
            pri = (acell)((uint32_t)pri << (FETCH() & 31));
            break;
        case OP_SHL_C_ALT:
            alt = (acell)((uint32_t)alt << (FETCH() & 31));
            break;
        case OP_SMUL:
            pri = (acell)((uint32_t)pri * (uint32_t)alt);
            break;
        case OP_SDIV: {
            if (pri == 0)
                FAIL(AVM_ERR_DIV);
            acell a = alt, b = pri, q, r;
            if (a == (acell)0x80000000 && b == -1) {
                q = a;
                r = 0;
            } else {
                q = a / b;
                r = a - q * b;      /* one divide, not two */
                if (r != 0 && ((r ^ b) < 0)) {
                    q--;
                    r += b;
                }
            }
            pri = q;
            alt = r;
            break;
        }
        case OP_ADD:
            pri = (acell)((uint32_t)pri + (uint32_t)alt);
            break;
        case OP_SUB:
            pri = (acell)((uint32_t)alt - (uint32_t)pri);
            break;
        case OP_AND:
            pri &= alt;
            break;
        case OP_OR:
            pri |= alt;
            break;
        case OP_XOR:
            pri ^= alt;
            break;
        case OP_NOT:
            pri = !pri;
            break;
        case OP_NEG:
            pri = (acell)(0u - (uint32_t)pri);
            break;
        case OP_INVERT:
            pri = ~pri;
            break;
        case OP_EQ:
            pri = (pri == alt);
            break;
        case OP_NEQ:
            pri = (pri != alt);
            break;
        case OP_SLESS:
            pri = (pri < alt);
            break;
        case OP_SLEQ:
            pri = (pri <= alt);
            break;
        case OP_SGRTR:
            pri = (pri > alt);
            break;
        case OP_SGEQ:
            pri = (pri >= alt);
            break;
        case OP_INC_PRI:
            pri++;
            break;
        case OP_INC_ALT:
            alt++;
            break;
        case OP_INC_I:
            if (!ADDR_OK(pri))
                FAIL(AVM_ERR_MEM);
            W(pri, R(pri) + 1);
            break;
        case OP_DEC_PRI:
            pri--;
            break;
        case OP_DEC_ALT:
            alt--;
            break;
        case OP_DEC_I:
            if (!ADDR_OK(pri))
                FAIL(AVM_ERR_MEM);
            W(pri, R(pri) - 1);
            break;
        case OP_MOVS: {
            int32_t n = FETCH();
            if (n > 0 && (!ADDR_OK(pri) || !ADDR_OK(pri + n - 1) ||
                          !ADDR_OK(alt) || !ADDR_OK(alt + n - 1)))
                FAIL(AVM_ERR_MEM);
            if (n > 0)
                memcpy(data + alt, data + pri, (size_t)n);
            break;
        }
        case OP_CMPS: {
            int32_t n = FETCH();
            if (n > 0 && (!ADDR_OK(pri) || !ADDR_OK(pri + n - 1) ||
                          !ADDR_OK(alt) || !ADDR_OK(alt + n - 1)))
                FAIL(AVM_ERR_MEM);
            pri = n > 0 ? memcmp(data + alt, data + pri, (size_t)n) : 0;
            break;
        }
        case OP_FILL: {
            int32_t n = FETCH();
            if (n > 0 && (!ADDR_OK(alt) || !ADDR_OK(alt + n - 1)))
                FAIL(AVM_ERR_MEM);
            for (int32_t i = 0; i + 4 <= n; i += 4)
                *(acell *)(data + alt + i) = pri;
            break;
        }
        case OP_HALT: {
            int32_t e = FETCH();
            if (retval)
                *retval = pri;
            vm->error = e;
            SYNC();
            if (e == AVM_SLEEP)
                return AVM_SLEEP;
            return e == 0 ? AVM_OK : -e;
        }
        case OP_BOUNDS:
            if ((uint32_t)pri > (uint32_t)FETCH())
                FAIL(AVM_ERR_BOUNDS);
            break;
        case OP_SYSREQ: {
            int32_t idx = FETCH();
            if (idx < 0 || idx >= AVM_MAX_NATIVES || !vm->natives[idx])
                FAIL(AVM_ERR_NATIVE);
            SYNC();
            pri = vm->natives[idx](vm, (acell *)(data + stk));
            if (vm->error != 0) {
                int e = vm->error;
                SYNC();
                return e == AVM_SLEEP ? AVM_SLEEP : (e > 0 ? -e : e);
            }
            break;
        }
        case OP_SWITCH: {
            int32_t ct = JUMPREL(cip);
            int32_t n = C(ct + 4);
            int32_t target = JUMPREL(ct + 8);
            int32_t p = ct + 12;
            for (int32_t i = 0; i < n; i++, p += 8) {
                if (C(p) == pri) {
                    target = JUMPREL(p + 4);
                    break;
                }
            }
            cip = target;
            break;
        }
        case OP_SWAP_PRI: {
            acell t = R(stk);
            W(stk, pri);
            pri = t;
            break;
        }
        case OP_SWAP_ALT: {
            acell t = R(stk);
            W(stk, alt);
            alt = t;
            break;
        }
        case OP_BREAK:
            break;
        case OP_LIDX:
            if (!ADDR_OK((uint32_t)alt + (uint32_t)pri * 4))
                FAIL(AVM_ERR_MEM);
            pri = R((uint32_t)alt + (uint32_t)pri * 4);
            break;
        case OP_LIDX_B: {
            int32_t shift = FETCH();
            uint32_t o = (uint32_t)alt + ((uint32_t)pri << shift);
            if (!ADDR_OK(o))
                FAIL(AVM_ERR_MEM);
            pri = R(o);
            break;
        }
        case OP_IDXADDR:
            pri = alt + pri * 4;
            break;
        case OP_IDXADDR_B: {
            int32_t shift = FETCH();
            pri = alt + (pri << shift);
            break;
        }
        case OP_PUSH_C:
            PUSH(FETCH());
            break;
        case OP_PUSH:
            PUSH(R(FETCH()));
            break;
        case OP_PUSH_S:
            PUSH(R(frm + FETCH()));
            break;
        case OP_PUSH_ADR:
            PUSH(frm + FETCH());
            break;
        case OP_PUSHR_C:
            PUSH((acell)(uintptr_t)(data + FETCH()));
            break;
        case OP_PUSHR_S:
            PUSH((acell)(uintptr_t)(data + frm + FETCH()));
            break;
        case OP_PUSHR_ADR:
            PUSH((acell)(uintptr_t)(data + frm + FETCH()));
            break;
        case OP_SYSREQ_N: {
            int32_t idx = FETCH();
            int32_t bytes = FETCH();
            if (idx < 0 || idx >= AVM_MAX_NATIVES || !vm->natives[idx])
                FAIL(AVM_ERR_NATIVE);
            PUSH(bytes);
            SYNC();
            pri = vm->natives[idx](vm, (acell *)(data + stk));
            stk += bytes + 4;
            if (vm->error != 0) {
                int e = vm->error;
                SYNC();
                return e == AVM_SLEEP ? AVM_SLEEP : (e > 0 ? -e : e);
            }
            break;
        }
        default:
            FAIL(AVM_ERR_OPCODE);
        }
    }
}

