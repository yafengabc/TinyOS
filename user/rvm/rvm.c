#include "rvm.h"

/* Bounds-checking build.  Define RVM_NOCHECKS for the "trusted bytecode"
 * speed reference; the checked build is the one that should ship. */
#ifndef RVM_NOCHECKS
#define RVM_CHECK_MEM 1
#else
#define RVM_CHECK_MEM 0
#endif

int rvm_run(RVM *vm, const uint32_t *code, uint32_t words)
{
    uint32_t *r = vm->r;
    uint32_t *mem = vm->mem;
    uint32_t pc = 0;

    for (;;) {
        if (pc >= words)
            return -1;
        uint32_t ins = code[pc++];
        uint32_t op = ins & 0xffu;
        uint32_t d = (ins >> 8) & 0x3fu;
        uint32_t a = (ins >> 14) & 0x3fu;
        uint32_t b = (ins >> 20) & 0x3fu;

        switch (op) {
        case OP_HALT:
            return 0;
        case OP_MOV:
            r[d] = r[a];
            break;
        case OP_LI:
            if (pc >= words)
                return -1;
            r[d] = code[pc++];
            break;
        case OP_ADD:
            r[d] = r[a] + r[b];
            break;
        case OP_SUB:
            r[d] = r[a] - r[b];
            break;
        case OP_MUL:
            r[d] = r[a] * r[b];
            break;
        case OP_DIV:
            if (r[b] == 0)
                return -2;
            r[d] = (uint32_t)((int32_t)r[a] / (int32_t)r[b]);
            break;
        case OP_MOD:
            if (r[b] == 0)
                return -2;
            r[d] = (uint32_t)((int32_t)r[a] % (int32_t)r[b]);
            break;
        case OP_AND:
            r[d] = r[a] & r[b];
            break;
        case OP_OR:
            r[d] = r[a] | r[b];
            break;
        case OP_XOR:
            r[d] = r[a] ^ r[b];
            break;
        case OP_SHL:
            r[d] = r[a] << (r[b] & 31u);
            break;
        case OP_SHR:
            r[d] = r[a] >> (r[b] & 31u);
            break;
        case OP_SLT:
            r[d] = (uint32_t)((int32_t)r[a] < (int32_t)r[b]);
            break;
        case OP_SLE:
            r[d] = (uint32_t)((int32_t)r[a] <= (int32_t)r[b]);
            break;
        case OP_SEQ:
            r[d] = (r[a] == r[b]);
            break;
        case OP_SNE:
            r[d] = (r[a] != r[b]);
            break;
        case OP_JMP:
            if (pc >= words)
                return -1;
            pc = code[pc];
            break;
        case OP_JZ:
        case OP_JNZ: {
            if (pc >= words)
                return -1;
            uint32_t target = code[pc++];
            int take = (op == OP_JZ) ? (r[a] == 0) : (r[a] != 0);
            if (take) {
                if (target >= words)
                    return -1;
                pc = target;
            }
            break;
        }
        case OP_LOAD: {
            uint32_t idx = r[a] + r[b];
#if RVM_CHECK_MEM
            if (idx >= RVM_MEM_WORDS)
                return -3;
#endif
            r[d] = mem[idx];
            break;
        }
        case OP_STORE: {
            uint32_t idx = r[a] + r[b];
#if RVM_CHECK_MEM
            if (idx >= RVM_MEM_WORDS)
                return -3;
#endif
            mem[idx] = r[d];
            break;
        }
        case OP_PRINTI:
            if (vm->putint)
                vm->putint((int)r[a]);
            break;
        case OP_PRINTC:
            if (vm->putc)
                vm->putc((int)a);
            break;
        case OP_UPTIME:
            r[d] = vm->uptime ? vm->uptime() : 0;
            break;
        case OP_CALL:
            if (pc >= words)
                return -1;
            if (vm->sp >= RVM_CALL_DEPTH)
                return -4;
            vm->callstack[vm->sp++] = pc + 1;
            pc = code[pc];
            break;
        case OP_RET:
            if (vm->sp == 0)
                return -5;
            pc = vm->callstack[--vm->sp];
            break;
        case OP_ADDI:
            if (pc >= words)
                return -1;
            r[d] = r[a] + code[pc++];
            break;
        case OP_NOP:
            break;
        default:
            return -6;
        }
    }
}
