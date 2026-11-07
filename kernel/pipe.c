#include "pipe.h"
#include "cpu.h"
#include "heap.h"
#include "platform.h"
#include "string.h"

/* Ring buffer with one reader end and one writer end (multiple tasks may
 * hold each end; the buffer itself does not care).  See pipe.h for the
 * division of labour with process.c and for the locking rule: the ring is
 * covered by proc_lock, the reference counters have a lock of their own. */

struct pipe {
    uint32_t head;          /* next byte to read  */
    uint32_t tail;          /* next byte to write */
    uint32_t count;         /* bytes in the buffer */
    uint32_t size;
    uint32_t readers;       /* open read ends  */
    uint32_t writers;       /* open write ends */
    /* Only readers/writers are covered by this.  They are the one part of a
     * pipe that is touched from outside proc_lock: fd_clone() takes a
     * reference while exec() is building the child's table, on whichever
     * CPU called exec, and a plain ++ there loses an increment against the
     * other core's -- when a child exits.  A lost writer reference is what
     * makes readers see EOF while the writer is still running: measured on
     * the RP2350, `piperace 4 32` stopped both ends of one pair at the same
     * 17 KiB on one run in four (OPEN-ITEMS §24). */
    spin_t   lock;
    uint8_t  buf[PIPE_SIZE];
};

struct pipe *pipe_create(void)
{
    struct pipe *p = kmalloc(sizeof(*p));

    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->size = PIPE_SIZE;
    return p;
}

/* The reference counters are the one part of a pipe touched from outside
 * proc_lock: fd_clone() takes a reference while exec() builds the child's
 * table, on whichever CPU called exec.  Interrupts off as well as the lock,
 * because the other side of this lock is fd_close_in(), which holds
 * proc_lock: with them on, a tick landing here would ask for proc_lock
 * while the CPU holding it waits for p->lock - both cores stuck (the same
 * ABBA shape as OPEN-ITEMS §23.3). */
void pipe_ref_reader(struct pipe *p)
{
    uint32_t irq = plat_irq_save();

    spin_lock(&p->lock);
    p->readers++;
    spin_unlock(&p->lock);
    plat_irq_restore(irq);
}

void pipe_ref_writer(struct pipe *p)
{
    uint32_t irq = plat_irq_save();

    spin_lock(&p->lock);
    p->writers++;
    spin_unlock(&p->lock);
    plat_irq_restore(irq);
}

int pipe_drop_reader(struct pipe *p)
{
    uint32_t irq = plat_irq_save();
    int last;

    spin_lock(&p->lock);
    /* count never underflows: a caller can only drop a reference it took. */
    p->readers = p->readers ? p->readers - 1 : 0;
    last = p->readers == 0 && p->writers == 0;
    spin_unlock(&p->lock);
    plat_irq_restore(irq);
    return last;
}

int pipe_drop_writer(struct pipe *p)
{
    uint32_t irq = plat_irq_save();
    int last;

    spin_lock(&p->lock);
    p->writers = p->writers ? p->writers - 1 : 0;
    last = p->writers == 0 && p->readers == 0;
    spin_unlock(&p->lock);
    plat_irq_restore(irq);
    return last;
}

/* Same rule: never take p->lock with interrupts on. */
uint32_t pipe_readers(struct pipe *p)
{
    uint32_t irq = plat_irq_save();
    uint32_t n;

    spin_lock(&p->lock);
    n = p->readers;
    spin_unlock(&p->lock);
    plat_irq_restore(irq);
    return n;
}

uint32_t pipe_writers(struct pipe *p)
{
    uint32_t irq = plat_irq_save();
    uint32_t n;

    spin_lock(&p->lock);
    n = p->writers;
    spin_unlock(&p->lock);
    plat_irq_restore(irq);
    return n;
}

int pipe_read(struct pipe *p, void *buf, uint32_t len)
{
    uint8_t *dst = buf;
    uint32_t n = 0;

    if (len == 0)
        return 0;
    if (p->count == 0)
        return p->writers ? PIPE_EAGAIN : 0;    /* empty, or EOF */

    while (n < len && p->count > 0) {
        dst[n++] = p->buf[p->head];
        p->head = (p->head + 1) % p->size;
        p->count--;
    }
    return (int)n;
}

int pipe_write(struct pipe *p, const void *buf, uint32_t len)
{
    const uint8_t *src = buf;
    uint32_t n = 0;

    if (len == 0)
        return 0;
    if (p->readers == 0)
        return PIPE_EPIPE;                      /* nobody left to read */

    while (n < len && p->count < p->size) {
        p->buf[p->tail] = src[n++];
        p->tail = (p->tail + 1) % p->size;
        p->count++;
    }
    return n ? (int)n : PIPE_EAGAIN;
}

int pipe_readable(struct pipe *p)
{
    return p->count > 0 || p->writers == 0;
}

int pipe_writable(struct pipe *p)
{
    return p->readers > 0 && p->count < p->size;
}
