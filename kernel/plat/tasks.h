#ifndef TINYOS_PLAT_TASKS_H
#define TINYOS_PLAT_TASKS_H

#include "types.h"

/* ---- board-owned kernel threads -------------------------------------- */

/* Start board-owned kernel threads, just before the first user task.  The
 * process table, the scheduler and every CPU exist by now, but plat_init()
 * runs too early for that.  Weak no-op by default; a board whose device needs
 * its own thread creates it here (the RP2350 RV32 USB pump is the first).
 *
 * A board thread that owns a device asks process_ktask_create() to pin itself
 * to the hart that owns it; the scheduler will not pick it anywhere else. */
void plat_start_tasks(void);

/* Does this port enforce user-memory protection (MPU/PMP)?  The weak default
 * is 0: a port that cannot answer is assumed to have none, which is the
 * conservative answer for a test that expects a fault.  RP2040 (Cortex-M0+)
 * returns 0; the ARM M3/M33 ports and the RISC-V PMP ports return 1.
 * /proc/cpuinfo exposes it as "memprot: on|off". */
int plat_has_memprot(void);

#endif /* TINYOS_PLAT_TASKS_H */
