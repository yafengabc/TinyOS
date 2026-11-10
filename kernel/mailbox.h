#ifndef TINYOS_MAILBOX_H
#define TINYOS_MAILBOX_H

#include "types.h"

/* A tiny message pool, not shared memory.
 *
 * Four slots of 256 bytes live in the kernel; msg_send copies a message in,
 * msg_recv copies it out.  A slot carries the destination pid and a sequence
 * number, so recv can scan four entries for the oldest message without a
 * per-process queue.  Nothing is reserved per task: idle tasks cost nothing
 * and the pool is the same 1 KiB whether one task or eight are running.
 *
 * send is try-only (-1 when the pool is full); recv is try-only too and
 * returns MSG_EAGAIN when there is nothing, because the blocking loop lives
 * in user land (sleep 1 ms between calls).  That keeps the kernel out of the
 * scheduler: no wait state, no wake path, no frame handoff.
 *
 * When the config is off, these are stubs and the pool is not allocated. */

#ifndef TOS_MAILBOX
#define TOS_MAILBOX 0
#endif

int  mailbox_send(int dst, int src, const void *buf, int len);
int  mailbox_recv(int me, void *buf, int cap);
/* Free everything still queued for `pid` (a task that is going away). */
void mailbox_drop(int pid);

#endif
