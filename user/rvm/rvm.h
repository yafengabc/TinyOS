#ifndef RVM_H
#define RVM_H

#include <stdint.h>

/* A small register VM for TinyOS scripting experiments.
 *
 * Instruction word: opcode:8 | dst:6 | a:6 | b:6.  64 registers, 2048 words
 * of data memory (8 KiB).  Immediates and branch targets are a second word.
 * The 6-bit register fields make register indices valid by construction; only
 * memory indices and the program counter need a range check.
 */

#define RVM_REGS      64
#define RVM_MEM_WORDS 2048
#define RVM_CALL_DEPTH 64

enum {
    OP_HALT = 0,
    OP_MOV,
    OP_LI,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_SHL,
    OP_SHR,
    OP_SLT,
    OP_SLE,
    OP_SEQ,
    OP_SNE,
    OP_JMP,
    OP_JZ,
    OP_JNZ,
    OP_LOAD,
    OP_STORE,
    OP_PRINTI,
    OP_PRINTC,
    OP_UPTIME,
    OP_CALL,
    OP_RET,
    OP_ADDI,
    OP_NOP
};

#define RVM_INS(op, d, a, b) \
    ((uint32_t)(op) | ((uint32_t)(d) << 8) | ((uint32_t)(a) << 14) | \
     ((uint32_t)(b) << 20))

typedef struct RVM {
    uint32_t r[RVM_REGS];
    uint32_t mem[RVM_MEM_WORDS];
    uint32_t callstack[RVM_CALL_DEPTH];
    uint32_t sp;
    const uint32_t *code;
    uint32_t words;
    uint32_t pc;
    void (*putint)(int);
    void (*putc)(int);
    uint32_t (*uptime)(void);
} RVM;

/* Returns 0 on HALT, a negative error code otherwise. */
int rvm_run(RVM *vm, const uint32_t *code, uint32_t words);

#endif
