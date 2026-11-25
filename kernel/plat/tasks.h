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

#endif /* TINYOS_PLAT_TASKS_H */
