/* Minimal OSI primitives for the ESP32-C3 WiFi blobs.
 *
 * The WiFi libraries are built against ESP-IDF's wifi_osi_funcs_t: they do
 * not call FreeRTOS directly, they call through this table.  Everything here
 * is built on top of the kernel-thread layer in process.c, so it runs in
 * M-mode on a ktask stack and gives the CPU back with process_ktask_sleep().
 *
 * The first version deliberately uses a 1 ms polling wait for queues,
 * semaphores and event groups.  That is slower than a proper wait list, but
 * it is small, has no lost-wakeup race, and scan/association timeouts are in
 * the tens of milliseconds - one tick of latency is not visible there.  Once
 * the driver is up, the wait lists can replace the polling without changing
 * the OSI contract.
 *
 * The table itself (wifi_osi_funcs_t) is added when the blobs are linked;
 * these primitives are kept separate so they can be exercised without the
 * binary libraries.
 */
#include "types.h"
#include "process.h"
#include "platform.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "wifi_osi.h"

#define OSI_BLOCKING 0xFFFFFFFFu

/* MMIO access for the few C3 registers the glue pokes. */
#define REG32(a) (*(volatile uint32_t *)(a))

/* FreeRTOS in ESP-IDF runs at 100 Hz, i.e. one tick is 10 ms; plat_ticks()
 * is 1 ms.  Blocking arguments are in ticks, process_ktask_sleep takes ms. */
#define OSI_TICK_MS 10u

uint32_t osi_get_free_heap_size(void);

/* The kernel and its heap live in SRAM through the instruction-bus alias
 * (0x4038_xxxx).  The WiFi MAC's DMA engine uses the data-bus alias, which
 * is exactly 0x0040_0000 lower (0x3FC8_xxxx).  Hand every blob allocation
 * to the driver through the data-bus address; the CPU can dereference that
 * alias too, so the same pointer works on both sides. */
#define WIFI_RAM_BASE   0x40380000u
#define WIFI_RAM_END    0x403E0000u
#define WIFI_DMA_DELTA  0x00700000u

void *wifi_dma_ptr(void *p)
{
    uintptr_t a = (uintptr_t)p;

    if (a >= WIFI_RAM_BASE && a < WIFI_RAM_END)
        return (void *)(a - WIFI_DMA_DELTA);
    return p;
}

void *wifi_cpu_ptr(void *p)
{
    uintptr_t a = (uintptr_t)p;

    if (a >= WIFI_RAM_BASE - WIFI_DMA_DELTA && a < WIFI_RAM_END - WIFI_DMA_DELTA)
        return (void *)(a + WIFI_DMA_DELTA);
    return p;
}

static volatile int wifi_in_isr;

/* ---- queues ---------------------------------------------------------- */

struct osi_queue {
    uint32_t len;
    uint32_t item;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint8_t *buf;
};

static int osi_queue_push(struct osi_queue *q, const void *item)
{
    if (q->count == q->len)
        return -1;
    memcpy(q->buf + q->tail * q->item, item, q->item);
    q->tail = (q->tail + 1) % q->len;
    q->count++;
    return 0;
}

static int osi_queue_pop(struct osi_queue *q, void *item)
{
    if (q->count == 0)
        return -1;
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->len;
    q->count--;
    return 0;
}

void *osi_queue_create(uint32_t len, uint32_t item)
{
    struct osi_queue *q;

    if (!len || !item)
        return NULL;
    q = kmalloc(sizeof(*q));
    if (!q) {
        kprintf("[osi] queue desc failed\n");
        return NULL;
    }
    q->buf = kmalloc(len * item);
    if (!q->buf) {
        kprintf("[osi] queue buf %u x %u failed\n", len, item);
        kfree(q);
        return NULL;
    }
    q->len = len;
    q->item = item;
    q->head = q->tail = q->count = 0;
    return q;
}

/* ESP-IDF's _wifi_create_queue returns a small descriptor whose first word
 * is the handle the other queue entry points take; net80211 reads that word
 * and stores it in its own global.  Keep the same shape. */
void osi_queue_delete(void *handle);

struct osi_wq {
    void *q;
};

void *osi_wifi_queue_create(int len, int item)
{
    struct osi_wq *w = kmalloc(sizeof(*w));

    if (!w)
        return NULL;
    w->q = osi_queue_create((uint32_t)len, (uint32_t)item);
    if (!w->q) {
        kfree(w);
        return NULL;
    }
    return w;
}

void osi_wifi_queue_delete(void *handle)
{
    struct osi_wq *w = handle;

    if (!w)
        return;
    osi_queue_delete(w->q);
    kfree(w);
}

void osi_queue_delete(void *handle)
{
    struct osi_queue *q = handle;

    if (!q)
        return;
    kfree(q->buf);
    kfree(q);
}

int osi_queue_send(void *handle, void *item, uint32_t ticks)
{
    struct osi_queue *q = handle;
    uint64_t deadline = 0;

    if (!q || !item)
        return 0;
    if (ticks && ticks != OSI_BLOCKING)
        deadline = plat_ticks() + (uint64_t)ticks * OSI_TICK_MS;
    for (;;) {
        uint32_t irq = plat_irq_save();
        int rc = osi_queue_push(q, item);

        plat_irq_restore(irq);
        if (rc == 0)
            return 1;
        if (ticks == 0)
            return 0;
        if (deadline && (int64_t)(plat_ticks() - deadline) >= 0)
            return 0;
        process_ktask_sleep(1);
    }
}

int osi_queue_send_from_isr(void *handle, void *item, void *hptw)
{
    struct osi_queue *q = handle;
    uint32_t irq;
    int rc;

    if (hptw)
        *(int *)hptw = 0;       /* pdFALSE: no higher-priority task woken */
    if (!q)
        return 0;
    irq = plat_irq_save();
    rc = osi_queue_push(q, item);
    plat_irq_restore(irq);
    return rc == 0 ? 1 : 0;
}

int osi_queue_recv(void *handle, void *item, uint32_t ticks)
{
    struct osi_queue *q = handle;
    uint64_t deadline = 0;

    if (!q || !item)
        return 0;
    if (ticks && ticks != OSI_BLOCKING)
        deadline = plat_ticks() + (uint64_t)ticks * OSI_TICK_MS;
    for (;;) {
        uint32_t irq = plat_irq_save();
        int rc = osi_queue_pop(q, item);

        plat_irq_restore(irq);
        if (rc == 0)
            return 1;
        if (ticks == 0)
            return 0;
        if (deadline && (int64_t)(plat_ticks() - deadline) >= 0)
            return 0;
        process_ktask_sleep(1);
    }
}

uint32_t osi_queue_msg_waiting(void *handle)
{
    struct osi_queue *q = handle;
    uint32_t n;
    uint32_t irq;

    if (!q)
        return 0;
    irq = plat_irq_save();
    n = q->count;
    plat_irq_restore(irq);
    return n;
}

/* ---- semaphores and mutexes ----------------------------------------- */

struct osi_sem {
    uint32_t count;
    uint32_t max;
    void *owner;                /* mutex only */
    uint8_t recursive;
    uint16_t depth;
};

void *osi_sem_create(uint32_t max, uint32_t init)
{
    struct osi_sem *s;

    s = kmalloc(sizeof(*s));
    if (!s) {
        kprintf("[osi] sem desc failed\n");
        return NULL;
    }
    s->count = init;
    s->max = max;
    s->owner = NULL;
    s->recursive = 0;
    s->depth = 0;
    return s;
}

void osi_sem_delete(void *handle)
{
    kfree(handle);
}

int osi_sem_take(void *handle, uint32_t ticks)
{
    struct osi_sem *s = handle;
    uint64_t deadline = 0;

    if (!s)
        return 0;
    if (ticks && ticks != OSI_BLOCKING)
        deadline = plat_ticks() + (uint64_t)ticks * OSI_TICK_MS;
    for (;;) {
        uint32_t irq = plat_irq_save();
        int got = s->count > 0;

        if (got)
            s->count--;
        plat_irq_restore(irq);
        if (got)
            return 1;
        if (ticks == 0)
            return 0;
        if (deadline && (int64_t)(plat_ticks() - deadline) >= 0)
            return 0;
        process_ktask_sleep(1);
    }
}

int osi_sem_give(void *handle)
{
    struct osi_sem *s = handle;
    uint32_t irq;
    int given = 0;

    if (!s)
        return 0;
    irq = plat_irq_save();
    if (s->count < s->max) {
        s->count++;
        given = 1;
    }
    plat_irq_restore(irq);
    return given;
}

/* ---- per-thread semaphores ------------------------------------------ *
 * net80211 posts work to its own task and then waits on a semaphore
 * belonging to the calling thread; the wifi task gives it when the work
 * item has been processed.  Keep one per process. */

struct osi_thread_sem {
    void *task;
    void *sem;
};

static struct osi_thread_sem thread_sems[8];

void *osi_wifi_thread_semphr_get(void)
{
    void *cur = (void *)(unsigned)process_current_pid();

    for (int i = 0; i < 8; i++)
        if (thread_sems[i].task && thread_sems[i].task == cur)
            return thread_sems[i].sem;
    for (int i = 0; i < 8; i++) {
        if (!thread_sems[i].task) {
            void *s = osi_sem_create(1, 0);

            if (!s)
                return NULL;
            thread_sems[i].sem = s;
            thread_sems[i].task = cur;
            return s;
        }
    }
    return NULL;
}

void *osi_mutex_create(int recursive)
{
    struct osi_sem *s = osi_sem_create(1, 1);

    if (s)
        s->recursive = recursive ? 1 : 0;
    return s;
}

void osi_mutex_delete(void *handle)
{
    osi_sem_delete(handle);
}

int osi_mutex_lock(void *handle)
{
    struct osi_sem *s = handle;
    void *me = (void *)(unsigned)process_current_pid();

    if (!s)
        return 0;
    if (s->recursive && s->owner == me) {
        s->depth++;
        return 1;
    }
    if (!osi_sem_take(handle, OSI_BLOCKING))
        return 0;
    s->owner = me;
    s->depth = 1;
    return 1;
}

int osi_mutex_unlock(void *handle)
{
    struct osi_sem *s = handle;
    void *me = (void *)(unsigned)process_current_pid();

    if (!s)
        return 0;
    if (s->recursive && s->owner == me) {
        if (s->depth > 1) {
            s->depth--;
            return 1;
        }
        s->owner = NULL;
        s->depth = 0;
    }
    return osi_sem_give(handle);
}

/* ---- event groups ---------------------------------------------------- */

struct osi_event {
    uint32_t bits;
};

void *osi_event_create(void)
{
    struct osi_event *e = kmalloc(sizeof(*e));

    if (e)
        e->bits = 0;
    return e;
}

void osi_event_delete(void *handle)
{
    kfree(handle);
}

uint32_t osi_event_set_bits(void *handle, uint32_t bits)
{
    struct osi_event *e = handle;
    uint32_t irq;
    uint32_t old;

    if (!e)
        return 0;
    irq = plat_irq_save();
    old = e->bits;
    e->bits |= bits;
    plat_irq_restore(irq);
    return old;
}

uint32_t osi_event_clear_bits(void *handle, uint32_t bits)
{
    struct osi_event *e = handle;
    uint32_t irq;
    uint32_t old;

    if (!e)
        return 0;
    irq = plat_irq_save();
    old = e->bits;
    e->bits &= ~bits;
    plat_irq_restore(irq);
    return old;
}

uint32_t osi_event_wait(void *handle, uint32_t bits, int clear,
                        int all, uint32_t ticks)
{
    struct osi_event *e = handle;
    uint64_t deadline = 0;

    if (!e)
        return 0;
    if (ticks && ticks != OSI_BLOCKING)
        deadline = plat_ticks() + (uint64_t)ticks * OSI_TICK_MS;
    for (;;) {
        uint32_t irq = plat_irq_save();
        uint32_t got = all ? ((e->bits & bits) == bits)
                           : ((e->bits & bits) != 0);
        uint32_t val = e->bits;

        if (got) {
            if (clear)
                e->bits &= ~bits;
            plat_irq_restore(irq);
            return val & bits;
        }
        plat_irq_restore(irq);
        if (ticks == 0)
            return 0;
        if (deadline && (int64_t)(plat_ticks() - deadline) >= 0)
            return 0;
        process_ktask_sleep(1);
    }
}

/* ---- tasks ----------------------------------------------------------- */

int osi_task_create(void (*fn)(void *), const char *name, uint32_t stack,
                    void *param, uint32_t prio, void *handle)
{
    int level;

    (void)handle;
    /* ESP-IDF priorities are 0..24 (higher number = more important); this
     * kernel has three levels where 0 is the most important.  Collapse. */
    if (prio >= 20)
        level = 0;
    else if (prio >= 10)
        level = 1;
    else
        level = 2;

    int pid = process_ktask_create(name, fn, param, stack, level);
    if (pid < 0) {
        kprintf("[osi] task %s stack %u prio %u failed\n", name, stack, prio);
        return 0;
    }
    /* xTaskCreate stores the new handle through the out parameter; net80211
     * later compares it with _task_get_current_task().  Pids are the
     * handles here. */
    if (handle)
        *(void **)handle = (void *)(unsigned)pid;
    return 1;                   /* pdPASS */
}

void osi_task_delete(void *handle)
{
    /* The WiFi tasks are long-lived and park on queues; there is no need to
     * tear one down for the scan-only milestone.  A real delete would set a
     * pending flag and let the task exit at its next blocking call. */
    (void)handle;
}

void osi_task_delay(uint32_t ticks)
{
    process_ktask_sleep(ticks ? ticks * OSI_TICK_MS : 1);
}

int osi_task_ms_to_tick(uint32_t ms)
{
    return (int)(ms / OSI_TICK_MS);
}

void *osi_task_get_current(void)
{
    return (void *)(unsigned)process_current_pid();
}

int osi_task_get_max_priority(void)
{
    return 25;                  /* configMAX_PRIORITIES in ESP-IDF */
}

void osi_task_yield_from_isr(void)
{
    /* A ktask is only preempted at its next trap/block; the ISR can mark a
     * task ready and the normal trap exit picks it up. */
}

bool osi_is_from_isr(void)
{
    return wifi_in_isr != 0;
}

static int spin_lock_token;

void *osi_spin_lock_create(void)
{
    /* Let the blob see a valid handle.  Interrupt masking is done by
     * osi_int_disable/osi_int_restore, so the token is only a cookie. */
    return &spin_lock_token;
}

void osi_spin_lock_delete(void *lock)
{
    (void)lock;
}

uint32_t osi_int_disable(void *lock)
{
    (void)lock;
    return plat_irq_save();
}

void osi_int_restore(void *lock, uint32_t state)
{
    (void)lock;
    plat_irq_restore(state);
}

void *osi_wifi_thread_semphr_get(void);

/* ---- interrupt registration ----------------------------------------- *
 * net80211 routes ETS_WIFI_MAC_INTR_SOURCE through the C3 interrupt
 * matrix and installs wDev_ProcessFiq as the handler.  The trap layer
 * calls wifi_irq_dispatch() for lines it does not recognise. */

#define INTMTX_BASE     0x600C2000u
#define INT_ENABLE_REG  (INTMTX_BASE + 0x104)
#define INT_PRI_REG(n)  (INTMTX_BASE + 0x114 + 4u * (n))
#define INT_THRESH_REG  (INTMTX_BASE + 0x194)

static void (*irq_handlers[32])(void *);
static void *irq_args[32];

void osi_set_isr(int n, void *f, void *arg)
{
    if (n < 0 || n >= 32)
        return;
    irq_handlers[n] = (void (*)(void *))f;
    irq_args[n] = arg;
}

void osi_set_intr(int32_t cpu, uint32_t source, uint32_t num, int32_t prio)
{
    (void)cpu;
    REG32(INTMTX_BASE + 4u * source) = num;
    REG32(INT_PRI_REG(num)) = (uint32_t)prio;
    REG32(INT_THRESH_REG) = 1;
}

void osi_ints_on(uint32_t mask)
{
    REG32(INT_ENABLE_REG) |= mask;
}

void osi_ints_off(uint32_t mask)
{
    REG32(INT_ENABLE_REG) &= ~mask;
}

static uint32_t irq_count;
static uint32_t irq_events;

int wifi_irq_dispatch(uint32_t line)
{
    if (line < 32 && irq_handlers[line]) {
        irq_count++;
        irq_events |= *(volatile uint32_t *)0x60033C3Cu;
        wifi_in_isr = 1;
        irq_handlers[line](irq_args[line]);
        wifi_in_isr = 0;
        return 1;
    }
    return 0;
}

uint32_t wifi_irq_total(void)
{
    return irq_count;
}

uint32_t wifi_irq_events(void)
{
    return irq_events;
}

/* ---- heap, time, randomness ----------------------------------------- */

void *osi_malloc(size_t n)
{
    void *p = kmalloc(n);
    if (!p)
        kprintf("[osi] malloc %u failed (free %u)\n", (unsigned)n, osi_get_free_heap_size());
    return wifi_dma_ptr(p);
}

void *osi_zalloc(size_t n)
{
    void *p = kmalloc(n);

    if (p)
        memset(p, 0, n);
    return wifi_dma_ptr(p);
}

void *osi_calloc(size_t n, size_t size)
{
    return osi_zalloc(n * size);
}

void *osi_realloc(void *p, size_t n)
{
    return wifi_dma_ptr(krealloc(wifi_cpu_ptr(p), n));
}

void osi_free(void *p)
{
    kfree(wifi_cpu_ptr(p));
}

uint32_t osi_get_free_heap_size(void)
{
    return heap_arena_bytes() - heap_used_bytes();
}

uint32_t osi_random(void)
{
    static uint32_t x = 0x12345678u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

int osi_get_random(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)osi_random();
    return 0;
}

int64_t osi_esp_timer_get_time(void)
{
    return (int64_t)(plat_now_ns() / 1000ull);
}

int osi_read_mac(uint8_t *mac, unsigned int type)
{
    static int printed;
    uint32_t lo, hi;

    if (!mac)
        return -1;
    lo = REG32(0x60008844u);            /* EFUSE_RD_MAC_SPI_SYS_0 */
    hi = REG32(0x60008848u) & 0xFFFFu;  /* EFUSE_RD_MAC_SPI_SYS_1 */
    /* The efuse holds the 48-bit MAC big-endian: hi is the first two bytes
     * (60:55), lo the remaining four (f9:73:0c:f0). */
    mac[0] = (uint8_t)(hi >> 8);
    mac[1] = (uint8_t)hi;
    mac[2] = (uint8_t)(lo >> 24);
    mac[3] = (uint8_t)(lo >> 16);
    mac[4] = (uint8_t)(lo >> 8);
    mac[5] = (uint8_t)lo;
    /* The factory MAC is the station MAC; the AP interface uses the next
     * address, as ESP-IDF does for the universal MAC scheme. */
    if (type == 1) {
        for (int i = 5; i >= 0; i--) {
            if (++mac[i] != 0)
                break;
        }
    }
    if (!printed) {
        printed = 1;
        kprintf("[wifi] mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return 0;
}

/* ---- stubs for the parts scan does not need yet ---------------------- */

/* PHY bring-up: the prebuilt libphy exposes register_chipv7_phy(), which
 * fills g_phyFuns and runs RF calibration.  Passing NULL selects libphy's
 * built-in default init data. */
typedef struct {
    uint8_t mac[6];
    uint8_t opaque[1894];
} wifi_phy_cal_t;

static wifi_phy_cal_t phy_cal;
extern const uint8_t wifi_phy_init_data[128];
extern int register_chipv7_phy(const void *init_data, void *cal_data, int mode);

/* C3 SYSCON: WiFi/BT clock gate and reset.  SYSCON lives at 0x60026000
 * (the SYSTEM block at 0x600C0000 is a different peripheral). */
#define SYSCON_WIFI_CLK_EN      0x60026014u
#define SYSCON_WIFI_RST_EN      0x60026018u
#define RTC_CNTL_DIG_PWC        0x60008088u
#define RTC_CNTL_DIG_ISO        0x6000808Cu
#define WIFI_BT_COMMON_CLK      0x0078078Fu
#define WIFI_MAC_CLK            (1u << 6)   /* not in the common mask */
#define WIFI_FORCE_PD           (1u << 17)
#define WIFI_FORCE_ISO          (1u << 28)
#define MODEM_RESET_FIELD       0x00002A1Fu

static void wifi_bt_common_clock_on(void)
{
    /* Common WiFi/BT gate group plus the MAC register clock.  Bit 6 is
     * missing from the header's "common" mask but is required for the MAC
     * core-ready handshake (hal_init). */
    REG32(SYSCON_WIFI_CLK_EN) |= WIFI_BT_COMMON_CLK | WIFI_MAC_CLK;
}

/* Same order as esp_wifi_bt_power_domain_on(): un-power-down, un-isolate
 * and pulse the modem reset field. */
void wifi_power_domain_on(void)
{
    REG32(RTC_CNTL_DIG_PWC) &= ~WIFI_FORCE_PD;
    for (volatile int i = 0; i < 4000; i++)
        ;
    wifi_bt_common_clock_on();
    REG32(SYSCON_WIFI_RST_EN) |= MODEM_RESET_FIELD;
    REG32(SYSCON_WIFI_RST_EN) &= ~MODEM_RESET_FIELD;
    REG32(RTC_CNTL_DIG_ISO) &= ~WIFI_FORCE_ISO;
    REG32(SYSCON_WIFI_CLK_EN) &= ~WIFI_BT_COMMON_CLK;
}

void osi_phy_enable(void)
{
    static int phy_done;
    int rc;

    if (phy_done)
        return;
    wifi_bt_common_clock_on();
    /* The factory station MAC goes into the calibration blob, as ESP-IDF
     * does; the RF calibration itself runs here on first enable. */
    memset(&phy_cal, 0, sizeof(phy_cal));
    phy_cal.mac[0] = 0x60; phy_cal.mac[1] = 0x55; phy_cal.mac[2] = 0xf9;
    phy_cal.mac[3] = 0x73; phy_cal.mac[4] = 0x0c; phy_cal.mac[5] = 0xf0;
    rc = register_chipv7_phy(wifi_phy_init_data, &phy_cal,
                             0 /* PHY_RF_CAL_PARTIAL */);
    kprintf("[wifi] phy init rc=%d\n", rc);
    phy_done = 1;
}

void osi_phy_disable(void) { }
int osi_phy_update_country_info(const char *country) { (void)country; return 0; }
void osi_wifi_clock_enable(void) { wifi_bt_common_clock_on(); }
void osi_wifi_clock_disable(void) { }
void osi_wifi_reset_mac(void)
{
    /* periph_module_reset(PERIPH_WIFI_MODULE): pulse the MAC reset.  The
     * ROM may leave it asserted, in which case hal_init waits forever for
     * the MAC to come out of reset. */
    REG32(SYSCON_WIFI_RST_EN) |= (1u << 2);     /* SYSTEM_WIFIMAC_RST */
    for (volatile int i = 0; i < 2000; i++)
        ;
    REG32(SYSCON_WIFI_RST_EN) &= ~(1u << 2);
}
void osi_wifi_rtc_enable_iso(void) { }
void osi_wifi_rtc_disable_iso(void) { }
void osi_dport_stall(void) { }
void osi_dport_unstall(void) { }
uint32_t osi_slowclk_cal_get(void) { return 0; }
int osi_event_post(const char *base, int32_t id, void *data, size_t size,
                   uint32_t ticks)
{
    (void)base; (void)id; (void)data; (void)size; (void)ticks;
    return 0;
}
int osi_coex_init(void) { return 0; }
void osi_coex_deinit(void) { }
int osi_coex_enable(void) { return 0; }
void osi_coex_disable(void) { }
uint32_t osi_coex_status_get(void) { return 0; }
void osi_coex_condition_set(uint32_t t, int d) { (void)t; (void)d; }
int osi_coex_wifi_request(uint32_t e, uint32_t l, uint32_t d)
{ (void)e; (void)l; (void)d; return 0; }
int osi_coex_wifi_release(uint32_t e) { (void)e; return 0; }
int osi_coex_wifi_channel_set(uint8_t p, uint8_t s) { (void)p; (void)s; return 0; }
int osi_coex_event_duration_get(uint32_t e, uint32_t *d) { (void)e; (void)d; return 0; }
int osi_coex_pti_get(uint32_t e, uint8_t *p) { (void)e; (void)p; return 0; }
void osi_coex_schm_status_bit_clear(uint32_t t, uint32_t s) { (void)t; (void)s; }
void osi_coex_schm_status_bit_set(uint32_t t, uint32_t s) { (void)t; (void)s; }
int osi_coex_schm_interval_set(uint32_t i) { (void)i; return 0; }
uint32_t osi_coex_schm_interval_get(void) { return 0; }
uint8_t osi_coex_schm_curr_period_get(void) { return 1; }
void *osi_coex_schm_curr_phase_get(void) { return NULL; }
int osi_coex_schm_process_restart(void) { return 0; }
int osi_coex_schm_register_cb(int t, int (*cb)(int)) { (void)t; (void)cb; return 0; }
int osi_coex_register_start_cb(int (*cb)(void)) { (void)cb; return 0; }
int osi_coex_schm_flexible_period_set(uint8_t p) { (void)p; return 0; }
uint8_t osi_coex_schm_flexible_period_get(void) { return 1; }
void *osi_coex_schm_get_phase_by_idx(int i) { (void)i; return NULL; }
int osi_nvs_get_blob(uint32_t h, const char *k, void *v, size_t *n)
{ (void)h; (void)k; (void)v; (void)n; return -1; }
int osi_nvs_set_blob(uint32_t h, const char *k, const void *v, size_t n)
{ (void)h; (void)k; (void)v; (void)n; return 0; }
int osi_nvs_open(const char *n, unsigned int m, uint32_t *h)
{ (void)n; (void)m; if (h) *h = 1; return 0; }
void osi_nvs_close(uint32_t h) { (void)h; }
int osi_nvs_commit(uint32_t h) { (void)h; return 0; }
int osi_nvs_erase_key(uint32_t h, const char *k) { (void)h; (void)k; return -1; }

/* ---- self-test (removed from the boot path after bring-up) ----------- */

static void *selftest_q;
static volatile int selftest_sum;
static volatile int selftest_done;

static void selftest_producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++)
        osi_queue_send(selftest_q, &i, OSI_BLOCKING);
    selftest_done = 1;
}

static void selftest_consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++) {
        int v = -1;

        if (osi_queue_recv(selftest_q, &v, OSI_BLOCKING) == 0)
            selftest_sum += v;
    }
    kprintf("[osi] queue selftest sum=%d (expect 4950)\n", selftest_sum);
}

void osi_selftest_start(void)
{
    selftest_q = osi_queue_create(8, sizeof(int));
    if (!selftest_q) {
        kprintf("[osi] queue create failed\n");
        return;
    }
    osi_task_create(selftest_producer, "osip", 4096, NULL, 0, NULL);
    osi_task_create(selftest_consumer, "osic", 4096, NULL, 0, NULL);
}

/* ---- wrappers with the exact signatures in wifi_osi_funcs_t ---------- */

static bool env_is_chip(void) { return true; }

static void intr_clear_noop(uint32_t src, uint32_t num) { (void)src; (void)num; }

static void *mutex_create_wrap(void) { return osi_mutex_create(0); }
static void *recursive_mutex_create_wrap(void) { return osi_mutex_create(1); }

static int32_t task_create_wrap(void *fn, const char *name, uint32_t stack,
                                void *param, uint32_t prio, void *handle)
{
    return osi_task_create((void (*)(void *))fn, name, stack, param, prio, handle);
}

static int32_t task_create_pinned_wrap(void *fn, const char *name,
                                       uint32_t stack, void *param,
                                       uint32_t prio, void *handle,
                                       uint32_t core)
{
    (void)core;
    return task_create_wrap(fn, name, stack, param, prio, handle);
}

static void task_delete_wrap(void *handle) { osi_task_delete(handle); }

struct osi_timer {
    void *obj;
    void (*fn)(void *);
    void *arg;
    uint64_t deadline;
    uint32_t period_ms;
    uint8_t armed;
    uint8_t repeat;
};

static struct osi_timer osi_timers[32];
static int osi_timer_task_started;

static void osi_timer_service(void *arg)
{
    (void)arg;
    for (;;) {
        void (*fns[32])(void *);
        void *args[32];
        int n = 0;
        uint32_t irq = plat_irq_save();
        uint64_t now = plat_ticks();

        for (int i = 0; i < 32; i++) {
            struct osi_timer *t = &osi_timers[i];

            if (!t->obj || !t->armed ||
                (int64_t)(now - t->deadline) < 0)
                continue;
            if (t->repeat)
                t->deadline = now + t->period_ms;
            else
                t->armed = 0;
            if (t->fn) {
                fns[n] = t->fn;
                args[n] = t->arg;
                n++;
            }
        }
        plat_irq_restore(irq);
        for (int i = 0; i < n; i++)
            fns[i](args[i]);
        process_ktask_sleep(1);
    }
}

static struct osi_timer *osi_timer_slot(void *obj, int create)
{
    struct osi_timer *free_slot = NULL;

    for (int i = 0; i < 32; i++) {
        if (osi_timers[i].obj == obj)
            return &osi_timers[i];
        if (!osi_timers[i].obj && !free_slot)
            free_slot = &osi_timers[i];
    }
    if (create && free_slot)
        free_slot->obj = obj;
    return create ? free_slot : NULL;
}

static void timer_setfn_wrap(void *timer, void *fn, void *arg)
{
    struct osi_timer *t = osi_timer_slot(timer, 1);
    uint32_t irq;

    if (!t)
        return;
    irq = plat_irq_save();
    t->fn = (void (*)(void *))fn;
    t->arg = arg;
    plat_irq_restore(irq);

    if (!osi_timer_task_started) {
        osi_timer_task_started = 1;
        process_ktask_create("wtimer", osi_timer_service, NULL, 4096, 0);
    }
}

static void timer_arm_ms(void *timer, uint32_t ms, int repeat)
{
    struct osi_timer *t = osi_timer_slot(timer, 1);
    uint32_t irq;

    if (!t)
        return;
    if (ms == 0)
        ms = 1;
    irq = plat_irq_save();
    t->deadline = plat_ticks() + ms;
    t->period_ms = ms;
    t->repeat = repeat ? 1 : 0;
    t->armed = 1;
    plat_irq_restore(irq);
}

static void timer_arm_wrap(void *timer, uint32_t tmout, bool repeat)
{
    timer_arm_ms(timer, tmout, repeat);
}

static void timer_arm_us_wrap(void *timer, uint32_t us, bool repeat)
{
    timer_arm_ms(timer, (us + 999u) / 1000u, repeat);
}

static void timer_disarm_wrap(void *timer)
{
    struct osi_timer *t = osi_timer_slot(timer, 0);

    if (t)
        t->armed = 0;
}

static void timer_done_wrap(void *timer)
{
    timer_disarm_wrap(timer);
}

static int nvs_get8_wrap(uint32_t h, const char *k, int8_t *v)
{ (void)h; (void)k; (void)v; return -1; }
static int nvs_getu8_wrap(uint32_t h, const char *k, uint8_t *v)
{ (void)h; (void)k; (void)v; return -1; }
static int nvs_getu16_wrap(uint32_t h, const char *k, uint16_t *v)
{ (void)h; (void)k; (void)v; return -1; }
static int nvs_set8_wrap(uint32_t h, const char *k, int8_t v)
{ (void)h; (void)k; (void)v; return 0; }
static int nvs_setu8_wrap(uint32_t h, const char *k, uint8_t v)
{ (void)h; (void)k; (void)v; return 0; }
static int nvs_setu16_wrap(uint32_t h, const char *k, uint16_t v)
{ (void)h; (void)k; (void)v; return 0; }

static int get_time_wrap(void *t) { (void)t; return 0; }

static void log_write_wrap(unsigned level, const char *tag, const char *fmt, ...)
{
    char buf[192];
    __builtin_va_list ap;

    (void)level; (void)tag;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    kprintf("[wlog] %s", buf);
}

static void log_writev_wrap(unsigned level, const char *tag,
                            const char *fmt, va_list args)
{
    char buf[192];

    (void)level; (void)tag;
    kvsnprintf(buf, sizeof(buf), fmt, args);
    kprintf("[wlog] %s", buf);
}

static void coex_condition_wrap(uint32_t type, bool dissatisfy)
{ osi_coex_condition_set(type, dissatisfy); }

wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._env_is_chip = env_is_chip,
    ._set_intr = osi_set_intr,
    ._clear_intr = intr_clear_noop,
    ._set_isr = osi_set_isr,
    ._ints_on = osi_ints_on,
    ._ints_off = osi_ints_off,
    ._is_from_isr = osi_is_from_isr,
    ._spin_lock_create = osi_spin_lock_create,
    ._spin_lock_delete = osi_spin_lock_delete,
    ._wifi_int_disable = osi_int_disable,
    ._wifi_int_restore = osi_int_restore,
    ._task_yield_from_isr = osi_task_yield_from_isr,
    ._semphr_create = osi_sem_create,
    ._semphr_delete = osi_sem_delete,
    ._semphr_take = osi_sem_take,
    ._semphr_give = osi_sem_give,
    ._wifi_thread_semphr_get = osi_wifi_thread_semphr_get,
    ._mutex_create = mutex_create_wrap,
    ._recursive_mutex_create = recursive_mutex_create_wrap,
    ._mutex_delete = osi_mutex_delete,
    ._mutex_lock = osi_mutex_lock,
    ._mutex_unlock = osi_mutex_unlock,
    ._queue_create = osi_queue_create,
    ._queue_delete = osi_queue_delete,
    ._queue_send = osi_queue_send,
    ._queue_send_from_isr = osi_queue_send_from_isr,
    ._queue_send_to_back = osi_queue_send,
    ._queue_send_to_front = osi_queue_send,
    ._queue_recv = osi_queue_recv,
    ._queue_msg_waiting = osi_queue_msg_waiting,
    ._event_group_create = osi_event_create,
    ._event_group_delete = osi_event_delete,
    ._event_group_set_bits = osi_event_set_bits,
    ._event_group_clear_bits = osi_event_clear_bits,
    ._event_group_wait_bits = osi_event_wait,
    ._task_create_pinned_to_core = task_create_pinned_wrap,
    ._task_create = task_create_wrap,
    ._task_delete = task_delete_wrap,
    ._task_delay = osi_task_delay,
    ._task_ms_to_tick = osi_task_ms_to_tick,
    ._task_get_current_task = osi_task_get_current,
    ._task_get_max_priority = osi_task_get_max_priority,
    ._malloc = osi_malloc,
    ._free = osi_free,
    ._event_post = osi_event_post,
    ._get_free_heap_size = osi_get_free_heap_size,
    ._rand = osi_random,
    ._dport_access_stall_other_cpu_start_wrap = osi_dport_stall,
    ._dport_access_stall_other_cpu_end_wrap = osi_dport_unstall,
    ._wifi_apb80m_request = osi_wifi_clock_enable,
    ._wifi_apb80m_release = osi_wifi_clock_disable,
    ._phy_disable = osi_phy_disable,
    ._phy_enable = osi_phy_enable,
    ._phy_update_country_info = osi_phy_update_country_info,
    ._read_mac = osi_read_mac,
    ._timer_arm = timer_arm_wrap,
    ._timer_disarm = timer_disarm_wrap,
    ._timer_done = timer_done_wrap,
    ._timer_setfn = timer_setfn_wrap,
    ._timer_arm_us = timer_arm_us_wrap,
    ._wifi_reset_mac = osi_wifi_reset_mac,
    ._wifi_clock_enable = osi_wifi_clock_enable,
    ._wifi_clock_disable = osi_wifi_clock_disable,
    ._wifi_rtc_enable_iso = osi_wifi_rtc_enable_iso,
    ._wifi_rtc_disable_iso = osi_wifi_rtc_disable_iso,
    ._esp_timer_get_time = osi_esp_timer_get_time,
    ._nvs_set_i8 = nvs_set8_wrap,
    ._nvs_get_i8 = nvs_get8_wrap,
    ._nvs_set_u8 = nvs_setu8_wrap,
    ._nvs_get_u8 = nvs_getu8_wrap,
    ._nvs_set_u16 = nvs_setu16_wrap,
    ._nvs_get_u16 = nvs_getu16_wrap,
    ._nvs_open = osi_nvs_open,
    ._nvs_close = osi_nvs_close,
    ._nvs_commit = osi_nvs_commit,
    ._nvs_set_blob = osi_nvs_set_blob,
    ._nvs_get_blob = osi_nvs_get_blob,
    ._nvs_erase_key = osi_nvs_erase_key,
    ._get_random = osi_get_random,
    ._get_time = get_time_wrap,
    ._random = (unsigned long (*)(void))osi_random,
    ._slowclk_cal_get = osi_slowclk_cal_get,
    ._log_write = log_write_wrap,
    ._log_writev = log_writev_wrap,
    ._log_timestamp = plat_uptime_ms,
    ._malloc_internal = osi_malloc,
    ._realloc_internal = osi_realloc,
    ._calloc_internal = osi_calloc,
    ._zalloc_internal = osi_zalloc,
    ._wifi_malloc = osi_malloc,
    ._wifi_realloc = osi_realloc,
    ._wifi_calloc = osi_calloc,
    ._wifi_zalloc = osi_zalloc,
    ._wifi_create_queue = osi_wifi_queue_create,
    ._wifi_delete_queue = osi_wifi_queue_delete,
    ._coex_init = osi_coex_init,
    ._coex_deinit = osi_coex_deinit,
    ._coex_enable = osi_coex_enable,
    ._coex_disable = osi_coex_disable,
    ._coex_status_get = osi_coex_status_get,
    ._coex_condition_set = coex_condition_wrap,
    ._coex_wifi_request = osi_coex_wifi_request,
    ._coex_wifi_release = osi_coex_wifi_release,
    ._coex_wifi_channel_set = osi_coex_wifi_channel_set,
    ._coex_event_duration_get = osi_coex_event_duration_get,
    ._coex_pti_get = osi_coex_pti_get,
    ._coex_schm_status_bit_clear = osi_coex_schm_status_bit_clear,
    ._coex_schm_status_bit_set = osi_coex_schm_status_bit_set,
    ._coex_schm_interval_set = osi_coex_schm_interval_set,
    ._coex_schm_interval_get = osi_coex_schm_interval_get,
    ._coex_schm_curr_period_get = osi_coex_schm_curr_period_get,
    ._coex_schm_curr_phase_get = osi_coex_schm_curr_phase_get,
    ._coex_schm_process_restart = osi_coex_schm_process_restart,
    ._coex_schm_register_cb = osi_coex_schm_register_cb,
    ._coex_register_start_cb = osi_coex_register_start_cb,
    ._coex_schm_flexible_period_set = osi_coex_schm_flexible_period_set,
    ._coex_schm_flexible_period_get = osi_coex_schm_flexible_period_get,
    ._coex_schm_get_phase_by_idx = osi_coex_schm_get_phase_by_idx,
    ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};
