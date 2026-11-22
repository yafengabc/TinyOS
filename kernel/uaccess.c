#include "uaccess.h"

/* The loader may only touch the task window it was handed.  Without an MMU a
 * corrupt or hostile image could otherwise overwrite the kernel through
 * p_vaddr / r_offset.  The bounds are passed in per call: two cores can exec()
 * at the same time, and a file-static pair would let one image be checked
 * against the other's window. */
int uaccess_range_ok(uint32_t lo, uint32_t hi, uint32_t va, uint32_t len)
{
    if (len == 0)
        return 1;
    if (va < lo)
        return 0;
    if (va + len < va) /* overflow */
        return 0;
    return va + len <= hi;
}
