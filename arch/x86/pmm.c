#include "pmm.h"
#include "string.h"

#define FRAME_COUNT ((PHYS_LIMIT - PHYS_START) / PAGE_SIZE)
#define BITMAP_BYTES (FRAME_COUNT / 8)

static uint8_t bitmap[BITMAP_BYTES];
static uint32_t used_frames;

void pmm_init(void)
{
    memset(bitmap, 0, sizeof(bitmap));
    used_frames = 0;
}

uint32_t pmm_alloc(void)
{
    for (uint32_t i = 0; i < FRAME_COUNT; i++) {
        uint32_t byte = i / 8;
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if (!(bitmap[byte] & bit)) {
            bitmap[byte] |= bit;
            used_frames++;
            uint32_t addr = PHYS_START + i * PAGE_SIZE;
            memset((void *)addr, 0, PAGE_SIZE);
            return addr;
        }
    }
    return 0;
}

void pmm_free(uint32_t addr)
{
    if (addr < PHYS_START || addr >= PHYS_LIMIT)
        return;
    if (addr & (PAGE_SIZE - 1))
        return;
    uint32_t i = (addr - PHYS_START) / PAGE_SIZE;
    uint32_t byte = i / 8;
    uint8_t bit = (uint8_t)(1u << (i % 8));
    if (bitmap[byte] & bit) {
        bitmap[byte] &= (uint8_t)~bit;
        if (used_frames)
            used_frames--;
    }
}

uint32_t pmm_free_bytes(void)
{
    return (FRAME_COUNT - used_frames) * PAGE_SIZE;
}
