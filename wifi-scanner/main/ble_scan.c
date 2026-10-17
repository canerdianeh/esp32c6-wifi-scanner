/*
    Bluetooth LE scanner (NimBLE); see ble_scan.h.
*/
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "ble_host.h"
#include "sdkconfig.h"
#include "ble_scan.h"
#include "demo.h"
#if CONFIG_SCANNER_DISPLAY
#include "ui.h"
#endif

static const char *TAG = "ble";

#define MAX_DEVS        64
#define REDRAW_MIN_MS   500

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ble_dev_t s_devs[MAX_DEVS];      /* guarded by s_mux */
static int s_num_devs;
static bool s_want_scan;
static uint32_t s_last_redraw_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

const char *ble_company_name(uint16_t c)
{
    switch (c) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x00E0: return "Google";
    case 0x0075: return "Samsung";
    case 0x0087: return "Garmin";
    case 0x0059: return "Nordic";
    case 0x02E5: return "Espressif";
    case 0x0171: return "Amazon";
    case 0x038F: return "Xiaomi";
    case 0x0157: return "Huami";
    case 0x0499: return "Ruuvi";
    case 0x0131: return "Cypress";
    case 0x000F: return "Broadcom";
    case 0x001D: return "Qualcomm";
    case 0x00D2: return "Dialog";
    case 0x0002: return "Intel";
    case 0x009E: return "Bose";
    case 0x00CD: return "Microchip";
    case 0x0046: return "MediaTek";
    case 0x0030: return "ST Micro";
    case 0x012D: return "Sony";
    case 0x00C4: return "LG";
    default:     return NULL;
    }
}

static void note_device(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    char name[sizeof(((ble_dev_t *)0)->name)] = "";
    uint16_t company = 0xFFFF;
    int8_t tx_power = BLE_TX_POWER_NONE;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0) {
        if (f.tx_pwr_lvl_is_present) {
            tx_power = f.tx_pwr_lvl;
        }
        if (f.name && f.name_len) {
            int n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
            memcpy(name, f.name, n);
            name[n] = 0;
            demo_sanitize(name, sizeof(name));
        }
        if (f.mfg_data && f.mfg_data_len >= 2) {
            company = f.mfg_data[0] | (f.mfg_data[1] << 8);
        }
    }

    uint8_t addr[6];     /* NimBLE stores addresses least significant byte first */
    for (int i = 0; i < 6; i++) {
        addr[i] = d->addr.val[5 - i];
    }

    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    ble_dev_t *dev = NULL;
    for (int i = 0; i < s_num_devs; i++) {
        if (memcmp(s_devs[i].addr, addr, 6) == 0) {
            dev = &s_devs[i];
            break;
        }
    }
    if (!dev) {
        if (s_num_devs < MAX_DEVS) {
            dev = &s_devs[s_num_devs++];
        } else {
            /* Full: reuse the one seen longest ago. */
            dev = &s_devs[0];
            for (int i = 1; i < s_num_devs; i++) {
                if (s_devs[i].last_seen_ms < dev->last_seen_ms) {
                    dev = &s_devs[i];
                }
            }
        }
        memset(dev, 0, sizeof(*dev));
        memcpy(dev->addr, addr, 6);
        dev->company = 0xFFFF;
        dev->tx_power = BLE_TX_POWER_NONE;
        dev->rssi_avg = d->rssi;
        dev->first_seen_ms = now;
    }
    dev->random_addr = d->addr.type != BLE_ADDR_PUBLIC;
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
        dev->connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                           d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
    }
    if (tx_power != BLE_TX_POWER_NONE) {
        dev->tx_power = tx_power;
    }
    /* Smooth over ~5 adverts; restart if the device was away for a while. */
    dev->rssi_avg = now - dev->last_seen_ms > 5000 ? d->rssi : 0.8f * dev->rssi_avg + 0.2f * d->rssi;
    dev->adverts++;
    if (name[0]) {
        strlcpy(dev->name, name, sizeof(dev->name));     /* scan responses carry the name; keep it */
    }
    if (company != 0xFFFF) {
        dev->company = company;
    }
    dev->rssi = d->rssi;
    dev->last_seen_ms = now;
    portEXIT_CRITICAL(&s_mux);

#if CONFIG_SCANNER_DISPLAY
    if (now - s_last_redraw_ms >= REDRAW_MIN_MS) {
        s_last_redraw_ms = now;
        ui_request_redraw();
    }
#endif
    static uint32_t s_last_log_ms;
    if (now - s_last_log_ms >= 10000) {
        s_last_log_ms = now;
        int recent = 0;
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < s_num_devs; i++) {
            recent += now - s_devs[i].last_seen_ms <= 30000;
        }
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "%d devices in the last 30 s", recent);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg);

static void start_discovery(void)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    struct ble_gap_disc_params p = {
        .itvl = 0x50,               /* 50 ms */
        .window = 0x40,             /* 40 ms */
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,               /* active: ask for scan responses (they carry names) */
        .filter_duplicates = 0,     /* keep reporting, so RSSI stays current */
    };
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Scan start failed (%d)", rc);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        note_device(&ev->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_want_scan) {
            start_discovery();
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    if (s_want_scan) {
        start_discovery();
    }
}

void ble_scan_start(void)
{
    s_want_scan = true;
    if (!ble_host_init()) {
        return;
    }
    ble_host_start();
    ble_host_on_sync(on_sync);      /* restarts scanning after a host reset */
    if (ble_host_synced()) {
        start_discovery();          /* already scanning is fine (BLE_HS_EALREADY) */
    }
    ESP_LOGI(TAG, "BLE scanning");
}

void ble_scan_stop(void)
{
    s_want_scan = false;
    if (ble_host_synced()) {
        ble_gap_disc_cancel();
    }
}

bool ble_scan_find(const uint8_t addr[6], ble_dev_t *out)
{
    bool found = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_num_devs; i++) {
        if (memcmp(s_devs[i].addr, addr, 6) == 0) {
            *out = s_devs[i];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

static int by_rssi(const void *a, const void *b)
{
    return ((const ble_dev_t *)b)->rssi - ((const ble_dev_t *)a)->rssi;
}

int ble_scan_get(ble_dev_t *out, int max, uint32_t max_age_ms)
{
    static ble_dev_t tmp[MAX_DEVS];     /* only the UI task calls this */
    uint32_t now = now_ms();
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_num_devs; i++) {
        if (now - s_devs[i].last_seen_ms <= max_age_ms) {
            tmp[n++] = s_devs[i];
        }
    }
    portEXIT_CRITICAL(&s_mux);
    qsort(tmp, n, sizeof(tmp[0]), by_rssi);
    int count = n < max ? n : max;
    memcpy(out, tmp, count * sizeof(tmp[0]));
    return n;
}
