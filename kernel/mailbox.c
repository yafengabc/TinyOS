/* The message pool behind SYS_MSG_SEND / SYS_MSG_RECV.
 *
 * The lock is private: the pool does not touch the process table except for
 * a liveness check on the destination, and that check takes proc_lock
 * itself, so the order is mb_lock -> proc_lock and never the other way.  The
 * copy is 256 bytes at most, under the lock, which is shorter than the
 * scheduler's own critical sections. */

#include "mailbox.h"
#include "process.h"
#include "platform.h"
#include "cpu.h"
#include "string.h"
#include "tos.h"

#if TOS_MAILBOX

#define MB_SLOTS 4
#define MB_SIZE  TOS_MSG_MAX

struct mb_msg {
    int      dst;
    int      src;
    int      len;
    uint32_t seq;
    uint8_t  data[MB_SIZE];
};

static struct mb_msg pool[MB_SLOTS];
static uint32_t next_seq;
static spin_t mb_lock = SPIN_INIT;

int mailbox_send(int dst, int src, const void *buf, int len)
{
    uint32_t irq;
    int slot = -1;

    if (len < 0 || len > MB_SIZE || !buf)
        return -1;
    if (dst <= 0 || !process_pid_exists(dst))
        return -1;

    irq = plat_spin_lock_irqsave(&mb_lock);
    for (int i = 0; i < MB_SLOTS; i++) {
        if (pool[i].dst == 0) {         /* 0 means free */
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        plat_spin_unlock_irqrestore(&mb_lock, irq);
        return -1;                      /* pool full: caller retries */
    }
    pool[slot].dst = dst;
    pool[slot].src = src;
    pool[slot].len = len;
    pool[slot].seq = next_seq++;
    memcpy(pool[slot].data, buf, (size_t)len);
    plat_spin_unlock_irqrestore(&mb_lock, irq);
    return 0;
}

int mailbox_recv(int me, void *buf, int cap)
{
    uint32_t irq;
    int best = -1;
    uint32_t bestseq = 0;
    int len;

    if (!buf || cap <= 0)
        return -1;

    irq = plat_spin_lock_irqsave(&mb_lock);
    for (int i = 0; i < MB_SLOTS; i++) {
        if (pool[i].dst != me)
            continue;
        if (best < 0 || pool[i].seq < bestseq) {
            best = i;
            bestseq = pool[i].seq;
        }
    }
    if (best < 0) {
        plat_spin_unlock_irqrestore(&mb_lock, irq);
        return MSG_EAGAIN;
    }
    if (cap < pool[best].len) {
        /* Leave it queued: the caller may come back with a bigger buffer. */
        plat_spin_unlock_irqrestore(&mb_lock, irq);
        return -1;
    }
    len = pool[best].len;
    memcpy(buf, pool[best].data, (size_t)len);
    pool[best].dst = 0;                 /* free */
    plat_spin_unlock_irqrestore(&mb_lock, irq);
    return len;
}

void mailbox_drop(int pid)
{
    uint32_t irq = plat_spin_lock_irqsave(&mb_lock);

    for (int i = 0; i < MB_SLOTS; i++)
        if (pool[i].dst == pid)
            pool[i].dst = 0;
    plat_spin_unlock_irqrestore(&mb_lock, irq);
}

#else  /* !TOS_MAILBOX: the syscalls exist, the pool does not */

int mailbox_send(int dst, int src, const void *buf, int len)
{
    (void)dst; (void)src; (void)buf; (void)len;
    return -1;
}

int mailbox_recv(int me, void *buf, int cap)
{
    (void)me; (void)buf; (void)cap;
    return MSG_EAGAIN;
}

void mailbox_drop(int pid) { (void)pid; }

#endif
