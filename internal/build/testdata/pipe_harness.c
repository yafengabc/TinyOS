/* Host harness for kernel/pipe.c.
 *
 * A pipe is a ring buffer plus two reference counters; the blocking half lives
 * in process.c.  The ring and the counters are testable as they are, with a
 * mock heap and a mock spinlock (single-threaded host, one core).
 *
 * PIPE_SIZE is 2048, so filling it needs a buffer that size; writing one more
 * byte must return PIPE_EAGAIN rather than corrupt the ring.
 */
#include <stdio.h>

#include "pipe.h"
#include "cpu.h"
#include "string.h"

/* --- the two kernel services pipe.c uses ------------------------------ */

static unsigned char arena[4][4096];
static int arena_used;

void *kmalloc(unsigned int n)
{
    if (n > sizeof arena[0] || arena_used >= 4) {
        return 0;
    }
    return arena[arena_used++];
}

void kfree(void *p)
{
    (void)p;            /* the arena is static; nothing to give back */
}

uint32_t plat_spin_lock_irqsave(spin_t *s)
{
    (void)s;
    return 0;
}

void plat_spin_unlock_irqrestore(spin_t *s, uint32_t irq)
{
    (void)s;
    (void)irq;
}

/* --- checks ------------------------------------------------------------ */

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    struct pipe *p = pipe_create();
    CHECK(p != 0, "pipe_create");
    pipe_ref_reader(p);
    pipe_ref_writer(p);

    unsigned char in[PIPE_SIZE + 16];
    unsigned char out[PIPE_SIZE + 16];
    for (int i = 0; i < (int)sizeof in; i++) {
        in[i] = (unsigned char)(i * 7 + 1);
    }

    CHECK(pipe_readable(p) == 0, "a fresh pipe is not readable");
    CHECK(pipe_writable(p) == 1, "a fresh pipe is writable");
    CHECK(pipe_read(p, out, 8) == PIPE_EAGAIN, "empty read is EAGAIN");

    CHECK(pipe_write(p, "hello", 5) == 5, "write 5");
    memset(out, 0, sizeof out);
    CHECK(pipe_read(p, out, 5) == 5, "read 5");
    CHECK(memcmp(out, "hello", 5) == 0, "read back the same bytes");

    /* Fill the ring exactly, then one byte more. */
    CHECK(pipe_write(p, in, PIPE_SIZE) == PIPE_SIZE, "fill the ring");
    CHECK(pipe_write(p, in, 1) == PIPE_EAGAIN, "full ring is EAGAIN");
    CHECK(pipe_read(p, out, PIPE_SIZE) == PIPE_SIZE, "drain the ring");
    CHECK(memcmp(out, in, PIPE_SIZE) == 0, "the ring kept every byte in order");

    /* Move head/tail off zero, then wrap across the end in one call. */
    CHECK(pipe_write(p, in, 1000) == 1000, "write 1000");
    CHECK(pipe_read(p, out, 1000) == 1000, "read 1000");
    CHECK(pipe_write(p, in, 1500) == 1500, "write across the end");
    memset(out, 0, sizeof out);
    CHECK(pipe_read(p, out, 1500) == 1500, "read across the end");
    CHECK(memcmp(out, in, 1500) == 0, "wrapped bytes are in order");

    /* EOF: the last writer goes away while the ring is empty. */
    pipe_drop_writer(p);
    CHECK(pipe_writers(p) == 0, "no writers left");
    CHECK(pipe_read(p, out, 1) == 0, "empty with no writers is EOF, not EAGAIN");
    CHECK(pipe_drop_reader(p) == 1, "last reader and writer: retire the pipe");

    /* EPIPE: a write with no reader left. */
    struct pipe *q = pipe_create();
    CHECK(q != 0, "second pipe_create");
    pipe_ref_writer(q);
    CHECK(pipe_write(q, "x", 1) == PIPE_EPIPE, "write with no readers is EPIPE");

    if (failures == 0) {
        printf("pipe ok\n");
    }
    return failures ? 1 : 0;
}
