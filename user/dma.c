#include "lib.h"

/* dma - exercise the DMA memory-to-memory path.
 *
 * There is no DMA file descriptor: this is a small self-test that fills a
 * buffer, asks the kernel to copy it with DMA, and verifies every byte.
 * A peripheral driver would use the same SYS_DMA_MEMCPY primitive for its
 * descriptors. */

int main(void)
{
    unsigned char src[256], dst[256];
    int n;

    for (int i = 0; i < (int)sizeof(src); i++) {
        src[i] = (unsigned char)(i * 37 + 11);
        dst[i] = 0;
    }

    n = dma_memcpy(dst, src, sizeof(src));
    if (n < 0) {
        puts("dma: this board has no DMA for user space\n");
        return 1;
    }
    if (n != (int)sizeof(src)) {
        printf("dma: short copy (%d/%d)\n", n, (int)sizeof(src));
        return 1;
    }
    for (int i = 0; i < (int)sizeof(src); i++) {
        if (dst[i] != src[i]) {
            printf("dma: mismatch at byte %d (0x%02x != 0x%02x)\n",
                   i, dst[i], src[i]);
            return 1;
        }
    }
    printf("dma: %d bytes copied and verified\n", n);
    return 0;
}
