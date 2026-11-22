#include "uaccess.h"

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
