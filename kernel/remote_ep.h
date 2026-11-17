#ifndef TINYOS_REMOTE_EP_H
#define TINYOS_REMOTE_EP_H

#include "types.h"

/* Remote endpoint (C7): one shape for the P4 LP core and the C6 companion.
 *
 * Context rules are part of the contract, not an implementation detail:
 *   - remote_ep_send/recv: task context only.
 *   - remote_ep_poll/link_state: task or tick context; may touch the lower.
 *   - remote_ep_irq: the only ISR entry; it sets `pending` and does no I/O.
 *     The tick drains it and wakes waiters with the state already updated.
 *
 * Capacity is per-lower (remote_ep_caps), never a macro: the LP mailbox and a
 * C6 SPI frame do not have the same limits.  A capability bit may only be
 * something a reader can ignore; anything that changes loading must bump a
 * format/version instead.
 *
 * v1 is polling-only (recv may spin to a deadline).  No callback shape until
 * a real lower proves what it needs. */

struct remote_ep_caps {
    uint32_t max_frame;    /* bytes per frame, header included */
    uint32_t queue_depth;  /* frames the lower can hold */
};

struct remote_ep_lower {
    const char *name;
    int (*link_state)(void);             /* 1 up, 0 down */
    int (*send)(const void *buf, uint32_t len);
    int (*recv)(void *buf, uint32_t cap);/* bytes or -1 when empty */
    struct remote_ep_caps (*caps)(void);
};

struct remote_ep {
    const struct remote_ep_lower *lower;
    volatile uint32_t pending;   /* set by remote_ep_irq(), drained by poll */
    uint32_t last_rx_tick;       /* liveness property, not a command */
    int link;
};

int  remote_ep_attach(struct remote_ep *ep, const struct remote_ep_lower *lo);
int  remote_ep_send(struct remote_ep *ep, const void *buf, uint32_t len);
int  remote_ep_recv(struct remote_ep *ep, void *buf, uint32_t cap,
                    uint32_t timeout_ms);
void remote_ep_irq(struct remote_ep *ep);
void remote_ep_poll(struct remote_ep *ep);
int  remote_ep_link_state(const struct remote_ep *ep);
uint32_t remote_ep_last_rx_tick(const struct remote_ep *ep);
struct remote_ep_caps remote_ep_get_caps(const struct remote_ep *ep);

/* Kernel self-test over the loopback mock lower.  Returns 0 or -1; called by
 * the SYS_REMOTE_TEST syscall so the QEMU/hardware runs can prove the shape
 * before a real lower exists. */
int remote_ep_selftest(void);

#endif
