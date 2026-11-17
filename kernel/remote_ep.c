#include "remote_ep.h"
#include "platform.h"
#include "cpu.h"
#include "string.h"

/* Loopback mock lower: a tiny frame queue with no hardware behind it.  It is
 * the first (and only) lower until the P4 LP mailbox or a C6 transport
 * exists, and it is what SYS_REMOTE_TEST exercises in QEMU. */

#define MOCK_FRAMES 4
#define MOCK_FRAME  64

static uint8_t  mock_q[MOCK_FRAMES][MOCK_FRAME];
static uint32_t mock_len[MOCK_FRAMES];
static uint32_t mock_head;
static uint32_t mock_tail;
static uint32_t mock_count;

/* send/recv are task context, and two cores can be in them at once: the mock
 * queue needs the same exclusion a real lower would.  irqsave (not just a
 * spin) because a tick landing on this core must not switch to another task
 * that re-enters the same queue. */
static spin_t mock_lock = SPIN_INIT;

static int mock_link_state(void)
{
    return 1;
}

static int mock_send(const void *buf, uint32_t len)
{
    uint32_t irq;

    if (!buf || len == 0 || len > MOCK_FRAME)
        return -1;
    irq = plat_spin_lock_irqsave(&mock_lock);
    if (mock_count == MOCK_FRAMES) {
        plat_spin_unlock_irqrestore(&mock_lock, irq);
        return -1;
    }
    memcpy(mock_q[mock_tail], buf, len);
    mock_len[mock_tail] = len;
    mock_tail = (mock_tail + 1u) % MOCK_FRAMES;
    mock_count++;
    plat_spin_unlock_irqrestore(&mock_lock, irq);
    return (int)len;
}

static int mock_recv(void *buf, uint32_t cap)
{
    uint32_t len, irq;

    if (!buf)
        return -1;
    irq = plat_spin_lock_irqsave(&mock_lock);
    if (mock_count == 0) {
        plat_spin_unlock_irqrestore(&mock_lock, irq);
        return -1;
    }
    len = mock_len[mock_head];
    if (len > cap) {                    /* leave the frame queued */
        plat_spin_unlock_irqrestore(&mock_lock, irq);
        return -1;
    }
    memcpy(buf, mock_q[mock_head], len);
    mock_head = (mock_head + 1u) % MOCK_FRAMES;
    mock_count--;
    plat_spin_unlock_irqrestore(&mock_lock, irq);
    return (int)len;
}

static struct remote_ep_caps mock_caps(void)
{
    struct remote_ep_caps c = { MOCK_FRAME, MOCK_FRAMES };
    return c;
}

static const struct remote_ep_lower mock_lower = {
    "mock-loopback", mock_link_state, mock_send, mock_recv, mock_caps
};

int remote_ep_attach(struct remote_ep *ep, const struct remote_ep_lower *lo)
{
    if (!ep || !lo || !lo->send || !lo->recv || !lo->link_state)
        return -1;
    ep->lower = lo;
    ep->pending = 0;
    ep->last_rx_tick = plat_ticks();
    ep->link = lo->link_state();
    return 0;
}

int remote_ep_send(struct remote_ep *ep, const void *buf, uint32_t len)
{
    if (!ep || !ep->lower)
        return -1;
    return ep->lower->send(buf, len);
}

int remote_ep_recv(struct remote_ep *ep, void *buf, uint32_t cap,
                   uint32_t timeout_ms)
{
    uint32_t hz, ticks, start;
    int r;

    if (!ep || !ep->lower)
        return -1;
    r = ep->lower->recv(buf, cap);
    if (r >= 0 || timeout_ms == 0)
        return r;

    /* v1 is polling only: no sleeping, no callbacks.  The deadline is an
     * upper bound on the poll, not a delay. */
    hz = plat_tick_hz();
    if (hz == 0)
        hz = 1000u;
    ticks = (timeout_ms * hz + 999u) / 1000u;
    if (ticks == 0)
        ticks = 1;
    start = plat_ticks();
    while ((uint32_t)(plat_ticks() - start) < ticks) {
        r = ep->lower->recv(buf, cap);
        if (r >= 0)
            return r;
    }
    return -1;
}

void remote_ep_irq(struct remote_ep *ep)
{
    /* ISR entry: no I/O, no lock, no lower call.  The tick owns the drain. */
    if (ep)
        ep->pending++;
}

void remote_ep_poll(struct remote_ep *ep)
{
    if (!ep || !ep->lower)
        return;
    if (ep->pending) {
        ep->pending = 0;
        ep->link = ep->lower->link_state();
        ep->last_rx_tick = plat_ticks();
    }
}

int remote_ep_link_state(const struct remote_ep *ep)
{
    return ep ? ep->link : 0;
}

uint32_t remote_ep_last_rx_tick(const struct remote_ep *ep)
{
    return ep ? ep->last_rx_tick : 0;
}

struct remote_ep_caps remote_ep_get_caps(const struct remote_ep *ep)
{
    if (ep && ep->lower && ep->lower->caps)
        return ep->lower->caps();
    {
        struct remote_ep_caps none = { 0, 0 };
        return none;
    }
}

int remote_ep_selftest(void)
{
    struct remote_ep ep;
    char buf[MOCK_FRAME];

    if (remote_ep_attach(&ep, &mock_lower) != 0)
        return -1;
    if (remote_ep_link_state(&ep) != 1)
        return -1;
    if (remote_ep_send(&ep, "one", 3) != 3)
        return -1;
    if (remote_ep_send(&ep, "two", 3) != 3)
        return -1;
    if (remote_ep_recv(&ep, buf, sizeof(buf), 0) != 3 || memcmp(buf, "one", 3) != 0)
        return -1;
    if (remote_ep_recv(&ep, buf, sizeof(buf), 0) != 3 || memcmp(buf, "two", 3) != 0)
        return -1;
    if (remote_ep_recv(&ep, buf, sizeof(buf), 0) != -1)
        return -1;

    /* The IRQ path must only set pending; poll updates liveness and link. */
    remote_ep_irq(&ep);
    if (ep.pending == 0)
        return -1;
    remote_ep_poll(&ep);
    if (ep.pending != 0)
        return -1;
    if (remote_ep_link_state(&ep) != 1)
        return -1;

    {
        struct remote_ep_caps c = remote_ep_get_caps(&ep);
        if (c.max_frame != MOCK_FRAME || c.queue_depth != MOCK_FRAMES)
            return -1;
    }
    return 0;
}
