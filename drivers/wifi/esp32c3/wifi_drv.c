/* WiFi bring-up glue.  This file is the only place that knows the
 * Espressif blob ABI; wifi_osi.c supplies the OS abstraction table.
 */
#include "types.h"
#include "process.h"
#include "platform.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "wifi_osi.h"
#include "devfs.h"

extern int esp_wifi_init_internal(const void *config);
extern int esp_wifi_set_mode(int mode);
extern int esp_wifi_start(void);
extern int esp_wifi_scan_start(const void *config, int block);
extern int esp_wifi_scan_get_ap_num(uint16_t *number);
extern int esp_wifi_scan_get_ap_records(uint16_t *number, void *records);
extern int esp_wifi_set_config(int ifx, const void *conf);
extern int esp_wifi_connect_internal(void);
extern int esp_wifi_sta_get_ap_info(void *ap_info);
extern void esp_wifi_internal_set_log_level(int level);
extern uint32_t osi_get_free_heap_size(void);
extern uint32_t wifi_irq_total(void);
extern uint32_t wifi_irq_events(void);
extern void wifi_power_domain_on(void);
void wifi_crypto_fill(uint32_t *raw);
void wifi_crypto_selftest(void);
void wifi_wpa_register(void);
int wifi_wpa_connected(void);

#ifndef WIFI_INIT_CONFIG_MAGIC
#define WIFI_INIT_CONFIG_MAGIC 0x1F2F3F4F
#endif

typedef struct { uint32_t raw[11]; } wpa_crypto_funcs_t;

/* Same field order as ESP-IDF's wifi_init_config_t.  wpa_crypto_funcs is
 * zeroed: scan does not use the WPA callbacks, only association does. */
typedef struct {
    wifi_osi_funcs_t *osi_funcs;
    wpa_crypto_funcs_t wpa_crypto_funcs;
    int static_rx_buf_num;
    int dynamic_rx_buf_num;
    int tx_buf_type;
    int static_tx_buf_num;
    int dynamic_tx_buf_num;
    int rx_mgmt_buf_type;
    int rx_mgmt_buf_num;
    int cache_tx_buf_num;
    int csi_enable;
    int ampdu_rx_enable;
    int ampdu_tx_enable;
    int amsdu_tx_enable;
    int nvs_enable;
    int nano_enable;
    int rx_ba_win;
    int wifi_task_core_id;
    int beacon_max_len;
    int mgmt_sbuf_num;
    uint64_t feature_caps;
    bool sta_disconnected_pm;
    int espnow_max_encrypt_num;
    int tx_hetb_queue_num;
    bool dump_hesigb_enable;
    int magic;
} tos_wifi_init_config_t;

static tos_wifi_init_config_t cfg;

static void wifi_init_config(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.osi_funcs = &g_wifi_osi_funcs;
    /* WPA2 callbacks: PBKDF2/HMAC/AES-CMAC from the C3 ROM accelerators. */
    wifi_crypto_fill(cfg.wpa_crypto_funcs.raw);
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 8;
    cfg.tx_buf_type = 1;
    cfg.static_tx_buf_num = 0;
    cfg.dynamic_tx_buf_num = 8;
    cfg.rx_mgmt_buf_type = 1;
    cfg.rx_mgmt_buf_num = 0;
    cfg.cache_tx_buf_num = 0;
    cfg.csi_enable = 0;
    cfg.ampdu_rx_enable = 1;
    cfg.ampdu_tx_enable = 1;
    cfg.amsdu_tx_enable = 0;
    cfg.nvs_enable = 0;        /* no NVS yet: calibration falls back */
    cfg.nano_enable = 0;
    cfg.rx_ba_win = 6;
    cfg.wifi_task_core_id = 0;
    cfg.beacon_max_len = 752;
    cfg.mgmt_sbuf_num = 32;
    cfg.feature_caps = 0;
    cfg.sta_disconnected_pm = false;
    cfg.espnow_max_encrypt_num = 7;
    cfg.tx_hetb_queue_num = 1;
    cfg.dump_hesigb_enable = false;
    cfg.magic = WIFI_INIT_CONFIG_MAGIC;
}

static volatile int wifi_scan_request;
static volatile int wifi_connect_request;
static char wifi_cfg_buf[160];
static int wifi_cfg_len;

static int wifi_init_once(void)
{
    int rc;

    wifi_crypto_selftest();
    wifi_init_config();

    /* IDF powers the WiFi/BT domain and pulses the modem reset before
     * esp_wifi_init_internal configures the MAC/PHY. */
    wifi_power_domain_on();

    rc = esp_wifi_init_internal(&cfg);
    kprintf("[wifi] init rc=%d free=%u\n", rc, osi_get_free_heap_size());
    if (rc != 0)
        return rc;
    rc = esp_wifi_set_mode(1);
    kprintf("[wifi] set_mode rc=%d\n", rc);
    if (rc != 0)
        return rc;
    wifi_wpa_register();
    rc = esp_wifi_start();
    kprintf("[wifi] start rc=%d\n", rc);
    if (rc == 0) {
        extern uint32_t g_log_level;

        /* Errors only: the driver's per-frame debug is far too chatty for
         * the shell.  Bump this to 7 when bringing up a new AP. */
        esp_wifi_internal_set_log_level(1);
        g_log_level = 1;
    }
    return rc;
}

int wifi_probe(void)
{
    return wifi_init_once();
}

static void wifi_scan(void)
{
    static uint8_t records[16 * 128];
    uint16_t n = 0;
    int rc;

    /* net80211 keeps the scan-result list head/tail in g_ic at +304/+308.
     * The vendor startup path initialises the tail to &head; ours does not
     * reach that step, so the first insertion linked the nodes somewhere
     * the copy-out never looked.  Reset both to the empty-list state before
     * each sweep. */
    {
        extern uint8_t g_ic[];

        *(uint32_t *)(g_ic + 304) = 0;
        *(uint32_t *)(g_ic + 308) = (uint32_t)(g_ic + 304);
    }

    rc = esp_wifi_scan_start(0, 1);
    kprintf("[wifi] scan rc=%d irq=%u ev=%x\n", rc, wifi_irq_total(),
            wifi_irq_events());
    if (rc != 0)
        return;

    rc = esp_wifi_scan_get_ap_num(&n);
    kprintf("[wifi] scan count rc=%d n=%u\n", rc, n);
    if (rc != 0 || n == 0)
        return;

    {
        uint16_t cnt = 16;
        int rrc = esp_wifi_scan_get_ap_records(&cnt, records);

        kprintf("[wifi] records rc=%d n=%u\n", rrc, cnt);
        /* sizeof(wifi_ap_record_t) is 92 in this blob (it memsets
         * count*92); ssid is at +6, primary at +39, rssi at +44. */
        for (unsigned i = 0; i < cnt && i < 16; i++) {
            uint8_t *r = records + i * 92;

            kprintf("[wifi]   %s ch=%u rssi=%d auth=%d pc=%d gc=%d %02x:%02x:%02x:%02x:%02x:%02x\n",
                    (char *)(r + 6), r[39], (int)(int8_t)r[44],
                    *(int *)(r + 48), *(int *)(r + 52), *(int *)(r + 56),
                    r[0], r[1], r[2], r[3], r[4], r[5]);
            kprintf("[wifi]     raw44:");
            for (int b = 44; b < 64; b++)
                kprintf(" %02x", r[b]);
            kprintf("\n");
        }
    }
}

static void wifi_try_connect(void)
{
    uint8_t conf[256];
    uint8_t info[92];
    char ssid[33], pass[65];
    int i = 0, si = 0, pi = 0;
    int rc;

    while (i < wifi_cfg_len && wifi_cfg_buf[i] != ' ' &&
           wifi_cfg_buf[i] != '\n' && si < 32)
        ssid[si++] = wifi_cfg_buf[i++];
    ssid[si] = 0;
    while (i < wifi_cfg_len && (wifi_cfg_buf[i] == ' ' || wifi_cfg_buf[i] == '\t'))
        i++;
    while (i < wifi_cfg_len && wifi_cfg_buf[i] != '\n' && pi < 64)
        pass[pi++] = wifi_cfg_buf[i++];
    pass[pi] = 0;
    if (!si) {
        kprintf("[wifi] connect: no ssid\n");
        return;
    }

    memset(conf, 0, sizeof(conf));
    memcpy(conf, ssid, (size_t)si);
    memcpy(conf + 32, pass, (size_t)pi);
    /* Leave the rest at IDF defaults: fast scan, sort by signal, RSSI floor
     * -127, authmode defaulted from the password length. */

    rc = esp_wifi_set_config(0, conf);
    kprintf("[wifi] set_config rc=%d ssid=%s\n", rc, ssid);
    if (rc != 0)
        return;
    rc = esp_wifi_connect_internal();
    kprintf("[wifi] connect rc=%d\n", rc);
    if (rc != 0)
        return;

    for (int t = 0; t < 40; t++) {
        rc = esp_wifi_sta_get_ap_info(info);
        if (rc == 0 && wifi_wpa_connected()) {
            kprintf("[wifi] connected %s ch=%u rssi=%d\n",
                    ssid, info[39], (int)(int8_t)info[44]);
            return;
        }
        process_ktask_sleep(500);
    }
    kprintf("[wifi] connect timeout rc=%d\n", rc);
}

/* /dev/wifi: ioctl(fd, 0) asks the control task to start one scan. */
static int wifi_dev_write(void *priv, uint32_t off, const void *buf, uint32_t len)
{
    (void)priv;
    (void)off;
    for (uint32_t i = 0; i < len && wifi_cfg_len < (int)sizeof(wifi_cfg_buf) - 1; i++)
        wifi_cfg_buf[wifi_cfg_len++] = ((const char *)buf)[i];
    wifi_cfg_buf[wifi_cfg_len] = 0;
    return (int)len;
}

static int wifi_dev_ioctl(void *priv, int cmd, int arg)
{
    (void)priv;
    (void)arg;
    if (cmd == 0) {
        wifi_scan_request = 1;
        return 0;
    }
    if (cmd == 1) {
        wifi_connect_request = 1;
        return 0;
    }
    return -1;
}

static const struct dev_ops wifi_ops = {
    .write = wifi_dev_write,
    .ioctl = wifi_dev_ioctl,
};

static void wifi_ctrl_task(void *arg)
{
    (void)arg;
    process_ktask_sleep(1000);
    if (wifi_init_once() != 0) {
        kprintf("[wifi] init failed\n");
        return;
    }
    devfs_register("dev/wifi", &wifi_ops, 0);
    kprintf("[wifi] ready\n");
    for (;;) {
        if (wifi_scan_request) {
            wifi_scan_request = 0;
            wifi_scan();
        }
        if (wifi_connect_request) {
            wifi_connect_request = 0;
            wifi_try_connect();
        }
        process_ktask_sleep(10);
    }
}

void wifi_test_start(void)
{
    process_ktask_create("wifi", wifi_ctrl_task, 0, 6144, 0);
}