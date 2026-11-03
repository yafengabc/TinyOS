#include "pipe.h"
#include "heap.h"
#include "string.h"

/* Ring buffer with one reader end and one writer end (multiple tasks may
 * hold each end; the buffer itself does not care).  See pipe.h for the
 * division of labour with process.c. */

struct pipe {
    uint32_t head;          /* next byte to read  */
    uint32_t tail;          /* next byte to write */
    uint32_t count;         /* bytes in the buffer */
    uint32_t size;
    uint32_t readers;       /* open read ends  */
    uint32_t writers;       /* open write ends */
    /* The lock is a no-op on a uniprocessor and a real spin lock when the
     * kernel is built for SMP; see kernel/cpu.h. */
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

void pipe_ref_reader(struct pipe *p)
{
    p->readers++;
}

void pipe_ref_writer(struct pipe *p)
{
    p->writers++;
}

int pipe_drop_reader(struct pipe *p)
{
    uint32_t r;

    /* count never underflows: a caller can only drop a reference it took. */
    r = p->readers ? --p->readers : 0;
    return r == 0 && p->writers == 0;
}

int pipe_drop_writer(struct pipe *p)
{
    uint32_t w;

    w = p->writers ? --p->writers : 0;
    return w == 0 && p->readers == 0;
}

uint32_t pipe_readers(struct pipe *p)
{
    return p->readers;
}

uint32_t pipe_writers(struct pipe *p)
{
    return p->writers;
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
