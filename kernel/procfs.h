#ifndef TINYOS_PROCFS_H
#define TINYOS_PROCFS_H

/* /proc: read-only views of kernel state, generated on open().
 *
 * These exist so that `ps`, `free` and `uptime` stop being shell builtins.
 * A builtin is code in the shell that the kernel cannot reuse and a script
 * cannot filter; `/proc/ps` is text that `cat` prints and, once there is a
 * grep, that a pipeline can select from. */

void procfs_init(void);

#endif
