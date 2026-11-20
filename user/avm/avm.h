#ifndef TINYOS_AVM_H
#define TINYOS_AVM_H

#include <stdint.h>

/* A small, AMX-compatible VM for Pawn 4.1 P-code.
 *
 * Scope: 32-bit cells, core instruction set (-O1), no overlay, no packed
 * opcodes, no 64-bit cells.  The VM keeps its state in a caller-provided
 * block; there is no malloc, no stdio and no longjmp.
 */

typedef int32_t acell;

struct avm;

typedef acell (*avm_native_fn)(struct avm *vm, acell *params);

typedef struct {
    const char *name;
    avm_native_fn fn;
} avm_native_info;

#define AVM_MAX_NATIVES 32

typedef struct avm {
    uint8_t *base;          /* AMX header */
    uint8_t *code;          /* base + hdr.cod */
    uint8_t *data;          /* base + hdr.dat */
    acell pri, alt;
    int32_t cip;            /* byte offset into code */
    int32_t frm, stk, hea, hlw, stp;   /* data-relative */
    int32_t code_size;
    avm_native_fn natives[AVM_MAX_NATIVES];
    const char *missing;
    int error;
    int slept;
} avm;

/* Return values.  Errors are negative; AVM_BUDGET and AVM_SLEEP are states
 * that can be resumed by calling avm_run() again. */
#define AVM_OK       0
#define AVM_BUDGET   1
#define AVM_SLEEP    12
#define AVM_ERR_FORMAT   (-1)
#define AVM_ERR_MEM      (-2)
#define AVM_ERR_BOUNDS   (-3)
#define AVM_ERR_DIV      (-4)
#define AVM_ERR_STACK    (-5)
#define AVM_ERR_NATIVE   (-6)
#define AVM_ERR_NOTFOUND (-7)
#define AVM_ERR_OPCODE   (-8)

int avm_init(avm *vm, void *block, uint32_t size,
             const avm_native_info *natives);
int avm_run(avm *vm, uint32_t budget, acell *retval);

/* Decode a Pawn string (packed or unpacked) into a NUL-terminated byte
 * buffer.  Returns the number of bytes written. */
int avm_getstring(char *dst, const acell *src, int cap);

/* Reconstruct a host pointer from a 32-bit cell.  On 32-bit targets this is
 * a plain cast; on a 64-bit host it restores the high bits from `data`, the
 * same trick amx_Address() uses. */
void *avm_address(avm *vm, acell a);

#endif
