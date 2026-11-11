#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "cpu.h"

#ifdef TOS_DMA

/* RP2040 DMA, memory-to-memory only for now.
 *
 * Each channel has a four-word working set (read address, write address,
 * transfer count, control).  Writing CTRL_TRIG with EN and TREQ_SEL =
 * PERMANENT starts a byte copy; BUSY stays set until the engine has consumed
 * the count.  The 12 channels are shared, so a simple spin lock serialises
 * this first user.  A future peripheral driver can claim a channel by
 * keeping its EN bit set; this allocator only picks a channel that is
 * neither enabled nor busy. */

#define DMA_CH(n)       (DMA_BASE + 0x40u * (uint32_t)(n))
#define DMA_READ(n)     (DMA_CH(n) + 0x00u)
#define DMA_WRITE(n)    (DMA_CH(n) + 0x04u)
#define DMA_COUNT(n)    (DMA_CH(n) + 0x08u)
#define DMA_CTRL(n)     (DMA_CH(n) + 0x0cu)

#define DMA_EN          (1u << 0)
#define DMA_INCR_READ   (1u << 4)
#define DMA_INCR_WRITE  (1u << 5)
#define DMA_TREQ_PERM   (0x3fu << 15)
#define DMA_BUSY        (1u << 24)
#define DMA_WRITE_ERR   (1u << 29)
#define DMA_READ_ERR    (1u << 30)

#define DMA_CHANNELS    12

static spin_t dma_lock = SPIN_INIT;
static int dma_ready;

static void dma_init_once(void)
{
    uint32_t v, spins;

    if (dma_ready)
        return;
    v = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    if (v & RESET_DMA) {
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, v & ~RESET_DMA);
        for (spins = 0; spins < 1000000u; spins++)
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
                RESET_DMA)
                break;
    }
    dma_ready = 1;
}

int plat_dma_memcpy(void *dst, const void *src, int len)
{
    uint32_t spins;
    int ch;

    if (len < 0 || (!dst && len > 0) || (!src && len > 0))
        return -1;
    if (len == 0)
        return 0;

    spin_lock(&dma_lock);
    dma_init_once();

    for (ch = 0; ch < DMA_CHANNELS; ch++) {
        uint32_t ctrl = mmio_read32(DMA_CTRL(ch));
        if (!(ctrl & (DMA_EN | DMA_BUSY)))
            break;
    }
    if (ch == DMA_CHANNELS) {
        spin_unlock(&dma_lock);
        return -1;
    }

    mmio_write32(DMA_READ(ch), (uint32_t)(uintptr_t)src);
    mmio_write32(DMA_WRITE(ch), (uint32_t)(uintptr_t)dst);
    mmio_write32(DMA_COUNT(ch), (uint32_t)len);
    mmio_write32(DMA_CTRL(ch),
                 DMA_EN | DMA_INCR_READ | DMA_INCR_WRITE | DMA_TREQ_PERM);

    for (spins = 0; spins < 100000000u; spins++) {
        uint32_t ctrl = mmio_read32(DMA_CTRL(ch));
        if (ctrl & (DMA_READ_ERR | DMA_WRITE_ERR)) {
            mmio_write32(DMA_CTRL(ch), 0);
            spin_unlock(&dma_lock);
            return -1;
        }
        if (!(ctrl & DMA_BUSY))
            break;
    }
    if (spins == 100000000u) {
        mmio_write32(DMA_CTRL(ch), 0);
        spin_unlock(&dma_lock);
        return -1;
    }

    mmio_write32(DMA_CTRL(ch), 0);      /* release EN for the next caller */
    spin_unlock(&dma_lock);
    return len;
}

#endif /* TOS_DMA */
