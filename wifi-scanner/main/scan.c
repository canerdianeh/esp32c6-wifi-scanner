/*
    Wearable Wi-Fi scanner

    Scans continuously and shows signal strength as a color on WS2812 / WS2814
    pixels. The signal is either the strongest access point in range or one
    specific SSID; see "Wearable Wi-Fi Scanner" in menuconfig.

    Based on the ESP-IDF Wi-Fi scan example (Public Domain / CC0).
*/
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "led_pixels.h"
#include "color_scale.h"
#include "ap_rows.h"
#include "demo.h"
#if CONFIG_SCANNER_PHONE_GPS
#include "ble_gps.h"
#endif
#include "wifi_scan.h"
#if CONFIG_SCANNER_DISPLAY
#include "wifi_loc.h"
#endif
#if CONFIG_SCANNER_FTM
#include "ftm.h"
#endif
#if CONFIG_SCANNER_DISPLAY
#include "gfx.h"
#include "ui.h"
#endif
#if CONFIG_SCANNER_GPS
#include "gps.h"
#endif
#if CONFIG_SCANNER_SD_LOG
#include "sdlog.h"
#endif

#define LED_FRAME_MS        20      /* 50 fps fade */
#define LED_REFRESH_FRAMES  50      /* resend unchanged color once a second, in case pixels were reconnected */
#define BOOT_TEST_MS        300

#if CONFIG_SCANNER_LED_ORDER_GRB
#define LED_ORDER LED_ORDER_GRB
#elif CONFIG_SCANNER_LED_ORDER_RGB
#define LED_ORDER LED_ORDER_RGB
#elif CONFIG_SCANNER_LED_ORDER_WRGB
#define LED_ORDER LED_ORDER_WRGB
#elif CONFIG_SCANNER_LED_ORDER_RGBW
#define LED_ORDER LED_ORDER_RGBW
#elif CONFIG_SCANNER_LED_ORDER_GRBW
#define LED_ORDER LED_ORDER_GRBW
#endif

#if CONFIG_SCANNER_ONBOARD_LED_ORDER_RGB
#define IS_ONBOARD_RGB 1
#else
#define IS_ONBOARD_RGB 0
#endif

#if CONFIG_SCANNER_LOG_APS
#define IS_LOG_APS 1
#else
#define IS_LOG_APS 0
#endif

#if CONFIG_SCANNER_DISPLAY
#define IS_DISPLAY 1
#else
#define IS_DISPLAY 0
#endif

static const char *TAG = "scanner";

static led_pixels_handle_t s_strips[2];
static int s_num_strips;

/* Color the LED task fades toward; written by the scan task. */
static portMUX_TYPE s_color_lock = portMUX_INITIALIZER_UNLOCKED;
static rgb_f_t s_target;

static void set_target_color(rgb_f_t c)
{
    taskENTER_CRITICAL(&s_color_lock);
    s_target = c;
    taskEXIT_CRITICAL(&s_color_lock);
}

static rgb_f_t get_target_color(void)
{
    taskENTER_CRITICAL(&s_color_lock);
    rgb_f_t c = s_target;
    taskEXIT_CRITICAL(&s_color_lock);
    return c;
}

static void show(led_color_t c)
{
    for (int i = 0; i < s_num_strips; i++) {
        led_pixels_fill(s_strips[i], c);
    }
}

/* Move `cur` toward `tgt` by at most `step`, keeping the channels in proportion so the hue doesn't drift mid-fade. */
static void fade_toward(rgb_f_t *cur, rgb_f_t tgt, float step)
{
    float dr = tgt.r - cur->r, dg = tgt.g - cur->g, db = tgt.b - cur->b;
    float d = fmaxf(fabsf(dr), fmaxf(fabsf(dg), fabsf(db)));
    if (d <= step) {
        *cur = tgt;
        return;
    }
    float k = step / d;
    cur->r += dr * k;
    cur->g += dg * k;
    cur->b += db * k;
}

static void led_task(void *arg)
{
    /* Red, green, blue at startup: if they appear in a different order, change "Pixel type / byte order". */
    static const uint32_t test_colors[] = { 0xFF0000, 0x00FF00, 0x0000FF };
    for (int i = 0; i < 3; i++) {
        show(color_to_led(color_from_hex(test_colors[i])));
        vTaskDelay(pdMS_TO_TICKS(BOOT_TEST_MS));
    }

    const float step = CONFIG_SCANNER_LED_FADE_MS > 0 ? 255.0f * LED_FRAME_MS / CONFIG_SCANNER_LED_FADE_MS : 255.0f;
    rgb_f_t cur = { 0 };
    led_color_t last = { 0 };
    int frames_since_send = LED_REFRESH_FRAMES;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        fade_toward(&cur, get_target_color(), step);
        led_color_t out = color_to_led(cur);
        if (memcmp(&out, &last, sizeof(out)) != 0 || ++frames_since_send >= LED_REFRESH_FRAMES) {
            show(out);
            last = out;
            frames_since_send = 0;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_FRAME_MS));
    }
}

static void build_scan_config(wifi_scan_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

#if CONFIG_SCANNER_MODE_TARGET_SSID
    /* Directed probes also find a hidden network with this SSID. */
    cfg->ssid = (uint8_t *)CONFIG_SCANNER_TARGET_SSID;
#elif CONFIG_SCANNER_SHOW_HIDDEN
    cfg->show_hidden = true;
#endif

#if CONFIG_SCANNER_SCAN_PASSIVE
    cfg->scan_type = WIFI_SCAN_TYPE_PASSIVE;
    cfg->scan_time.passive = CONFIG_SCANNER_DWELL_MS;
#else
    cfg->scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg->scan_time.active.min = CONFIG_SCANNER_DWELL_MS;
    cfg->scan_time.active.max = CONFIG_SCANNER_DWELL_MS;
#endif

#if CONFIG_SCANNER_USE_CHANNEL_LIST
    const char *p = CONFIG_SCANNER_CHANNEL_LIST;
    while (*p) {
        char *end;
        long ch = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        if (ch >= 1 && ch <= 14) {
            cfg->channel_bitmap.ghz_2_channels |= (1 << ch);
        } else {
            ESP_LOGW(TAG, "Ignoring invalid channel %ld in channel list", ch);
        }
        p = end;
    }
    if (cfg->channel_bitmap.ghz_2_channels == 0) {
        ESP_LOGW(TAG, "Channel list \"%s\" has no valid channels; scanning all", CONFIG_SCANNER_CHANNEL_LIST);
    }
#endif
}

static int by_rssi_desc(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

/* Fetch the scan results, strongest first. Always frees the driver's result list; caller frees the returned array. */
static wifi_ap_record_t *fetch_records(uint16_t *count)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = n ? malloc(n * sizeof(*recs)) : NULL;
    if (!recs) {
        esp_wifi_clear_ap_list();
        *count = 0;
        return NULL;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        *count = 0;
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        demo_sanitize((char *)recs[i].ssid, sizeof(recs[i].ssid));
    }
    qsort(recs, n, sizeof(*recs), by_rssi_desc);
    *count = n;
    return recs;
}

/* Strongest AP for automatic mode (or the configured target SSID), or NULL. `recs` is sorted strongest first. */
static const wifi_ap_record_t *pick_auto(const wifi_ap_record_t *recs, uint16_t n)
{
    for (int i = 0; i < n; i++) {
#if CONFIG_SCANNER_MODE_TARGET_SSID
        if (strcmp((const char *)recs[i].ssid, CONFIG_SCANNER_TARGET_SSID) != 0) {
            continue;
        }
#endif
        return &recs[i];
    }
    return NULL;
}

/* Shared by the scan task and the button task; guarded by s_lock. */
static SemaphoreHandle_t s_lock;
static wifi_ap_record_t *s_recs;    /* latest scan, strongest first */
static uint16_t s_num_recs;
static ap_rows_t s_rows;            /* the listed networks, in stable order */
static char s_selected[33];         /* SSID the LED is locked to; "" = automatic */
static float s_smoothed;
static bool s_tracking;
static int s_missed;

/* Choose the LED's network from the latest scan, set its color and redraw the display.
 * Call with s_lock held. `new_scan` is false when only the selection changed. */
static void refresh_outputs(bool new_scan)
{
    const float keep = CONFIG_SCANNER_RSSI_SMOOTHING / 100.0f;
    const char *name = NULL;
    int rssi = 0, channel = 0;

    if (s_selected[0]) {
        const ap_row_t *row = ap_rows_find(&s_rows, s_selected);
        if (row && row->count > 0) {
            name = row->ssid;
            rssi = row->rssi[0];
            channel = row->primary;
        }
    } else {
        const wifi_ap_record_t *best = pick_auto(s_recs, s_num_recs);
        if (best) {
            name = best->ssid[0] ? (const char *)best->ssid : "<hidden>";
            rssi = best->rssi;
            channel = best->primary;
        }
    }

    if (name) {
        s_smoothed = s_tracking ? keep * s_smoothed + (1.0f - keep) * rssi : rssi;
        s_tracking = true;
        s_missed = 0;
        rgb_f_t c = color_scale_for_rssi(s_smoothed);
        set_target_color(c);
        ESP_LOGI(TAG, "%s%s  %d dBm  ch %d  (smoothed %.1f)  color #%02X%02X%02X  [%u APs]",
                 s_selected[0] ? "[locked] " : "", name, rssi, channel, s_smoothed,
                 (int)c.r, (int)c.g, (int)c.b, s_num_recs);
    } else if (!new_scan || ++s_missed >= CONFIG_SCANNER_MISSED_SCANS) {
        if (s_tracking || !new_scan || s_missed == CONFIG_SCANNER_MISSED_SCANS) {
            ESP_LOGI(TAG, "%s not found", s_selected[0] ? s_selected : "Matching network");
        }
        s_tracking = false;
        set_target_color(color_from_hex(CONFIG_SCANNER_NOT_FOUND_COLOR));
    } else {
        ESP_LOGI(TAG, "Not seen this scan (%d/%d), holding color", s_missed, CONFIG_SCANNER_MISSED_SCANS);
    }

#if CONFIG_SCANNER_DISPLAY
    ui_request_redraw();
#endif
}

/* Pausing: another mode needs the radio. */
static TaskHandle_t s_scan_task;
static volatile bool s_pause_req;
static volatile bool s_paused;
static bool s_wifi_stopped;
static uint32_t s_scan_count;

static void scan_task(void *arg)
{
    wifi_scan_config_t cfg;
    build_scan_config(&cfg);
    set_target_color(color_from_hex(CONFIG_SCANNER_NOT_FOUND_COLOR));

    for (;;) {
        if (s_pause_req) {
            set_target_color((rgb_f_t) { 0 });
            s_tracking = false;
            ESP_LOGI(TAG, "Wi-Fi scanning paused");
            s_paused = true;
            while (s_pause_req) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            }
            s_paused = false;
            ESP_LOGI(TAG, "Wi-Fi scanning resumed");
            continue;
        }

#if CONFIG_SCANNER_PHONE_GPS
        /* With Bluetooth active the driver uses its own per-channel times (and warns about custom ones). */
        wifi_scan_config_t c = cfg;
        if (ble_gps_state() != BLE_GPS_OFF) {
            memset(&c.scan_time, 0, sizeof(c.scan_time));
        }
        esp_err_t err = esp_wifi_scan_start(&c, true);
#else
        esp_err_t err = esp_wifi_scan_start(&cfg, true);
#endif
        if (err != ESP_OK) {
            if (!s_pause_req) {
                ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_pause_req) {
            esp_wifi_clear_ap_list();
            continue;
        }

        uint16_t n;
        wifi_ap_record_t *recs = fetch_records(&n);
        for (int i = 0; i < n; i++) {
            bool listed = ap_rows_accepts(&recs[i]);
            if (!IS_LOG_APS && (listed || !IS_DISPLAY)) {
                continue;
            }
            const uint8_t *m = recs[i].bssid;
            ESP_LOGI(TAG, "  %s%-32s %4d dBm  ch %2d  %02x:%02x:%02x:%02x:%02x:%02x",
                     (IS_DISPLAY && !listed) ? "not shown: " : "",
                     recs[i].ssid[0] ? (char *)recs[i].ssid : "<hidden>", recs[i].rssi, recs[i].primary,
                     m[0], m[1], m[2], m[3], m[4], m[5]);
        }

#if CONFIG_SCANNER_SD_LOG
        sdlog_scan(recs, n);
#endif

        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_recs);
        s_recs = recs;
        s_num_recs = n;
        s_scan_count++;
        ap_rows_update(&s_rows, recs, n);
        refresh_outputs(true);
        xSemaphoreGive(s_lock);

#if CONFIG_SCANNER_DISPLAY
        /* ANQP location queries for the network open in Wi-Fi details, between scans. */
        if (!s_pause_req) {
            wifi_loc_run_queries();
        }
#endif
#if CONFIG_SCANNER_FTM
        /* FTM ranging for the network open in Wi-Fi details, between scans. */
        if (!s_pause_req) {
            ftm_run_pending();
        }
#endif

#if CONFIG_SCANNER_SCAN_INTERVAL_MS > 0
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SCANNER_SCAN_INTERVAL_MS));
#endif
    }
}

void wifi_scan_pause(bool stop_wifi)
{
#if CONFIG_SCANNER_DISPLAY
    esp_wifi_set_promiscuous(false);    /* stop the beacon sniffer so other radios get the air */
#endif
    s_pause_req = true;
    esp_wifi_scan_stop();
    for (int i = 0; i < 100 && !s_paused; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (stop_wifi && !s_wifi_stopped) {
        esp_wifi_stop();
        s_wifi_stopped = true;
    }
}

void wifi_scan_resume(void)
{
    if (s_wifi_stopped) {
        esp_wifi_start();
        s_wifi_stopped = false;
    }
    if (s_pause_req) {
        s_pause_req = false;
        xTaskNotifyGive(s_scan_task);
    }
#if CONFIG_SCANNER_DISPLAY
    esp_wifi_set_promiscuous(true);     /* beacon sniffer back on (location elements) */
#endif
}

void wifi_ui_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void wifi_ui_unlock(void)
{
    xSemaphoreGive(s_lock);
}

const ap_rows_t *wifi_ui_rows(void)
{
    return &s_rows;
}

const char *wifi_ui_selected(void)
{
    return s_selected;
}

uint32_t wifi_ui_scan_count(void)
{
    return s_scan_count;
}

/* Lock the LED to the next listed SSID (after the last one, back to automatic). */
void wifi_ui_select_next(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;   /* automatic, or the selected SSID has dropped off the list: start at the top */
    for (int i = 0; s_selected[0] && i < s_rows.n; i++) {
        if (strcmp(s_rows.rows[i].ssid, s_selected) == 0) {
            idx = i;
        }
    }
    if (idx + 1 < s_rows.n) {
        strlcpy(s_selected, s_rows.rows[idx + 1].ssid, sizeof(s_selected));
        ESP_LOGI(TAG, "LED follows \"%s\"", s_selected);
    } else {
        s_selected[0] = 0;
        ESP_LOGI(TAG, "LED follows the strongest network");
    }
    s_tracking = false;     /* jump straight to the new network's level */
    refresh_outputs(false);
    xSemaphoreGive(s_lock);
}

void wifi_ui_select(const char *ssid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(s_selected, ssid) != 0) {
        strlcpy(s_selected, ssid, sizeof(s_selected));
        if (ssid[0]) {
            ESP_LOGI(TAG, "LED follows \"%s\"", s_selected);
        } else {
            ESP_LOGI(TAG, "LED follows the strongest network");
        }
        s_tracking = false;
        refresh_outputs(false);
    }
    xSemaphoreGive(s_lock);
}

int wifi_ui_records(const wifi_ap_record_t **recs)
{
    *recs = s_recs;
    return s_num_recs;
}

const char *wifi_auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_OWE:             return "OWE";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_ENTERPRISE:      return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-ENT";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/WPA3-ENT";
    case WIFI_AUTH_WPA3_ENT_192:    return "WPA3-ENT-192";
    case WIFI_AUTH_DPP:             return "DPP";
    default:                        return "OTHER";
    }
}

void led_show_rssi(float rssi)
{
    set_target_color(color_scale_for_rssi(rssi));
}

void led_show_off(void)
{
    set_target_color((rgb_f_t) { 0 });
}

/* Back to automatic, and rebuild the list freshly sorted. */
void wifi_ui_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_selected[0] = 0;
    ap_rows_reset(&s_rows);
    if (s_num_recs) {
        ap_rows_update(&s_rows, s_recs, s_num_recs);
        s_tracking = false;
        ESP_LOGI(TAG, "List re-sorted, LED follows the strongest network");
        refresh_outputs(false);
    }
    xSemaphoreGive(s_lock);
}

#if CONFIG_SCANNER_BUTTON && !CONFIG_SCANNER_DISPLAY
/* Without a display: short press = next network, long press = back to automatic and re-sort. */
#define BUTTON_POLL_MS      20
#define BUTTON_DEBOUNCE_MS  40

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_SCANNER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    bool long_done = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        if (gpio_get_level(CONFIG_SCANNER_BUTTON_GPIO) == 0) {
            held_ms += BUTTON_POLL_MS;
            if (!long_done && held_ms >= CONFIG_SCANNER_BUTTON_LONG_MS) {
                long_done = true;
                wifi_ui_reset();
            }
        } else {
            if (held_ms >= BUTTON_DEBOUNCE_MS && !long_done) {
                wifi_ui_select_next();
            }
            held_ms = 0;
            long_done = false;
        }
    }
}
#endif

static void add_strip(int gpio, uint16_t count, led_order_t order, const char *name)
{
    esp_err_t err = led_pixels_new(gpio, count, order, &s_strips[s_num_strips]);
    if (err == ESP_OK) {
        s_num_strips++;
        ESP_LOGI(TAG, "%s: %u pixel(s) on GPIO%d", name, count, gpio);
    } else {
        ESP_LOGE(TAG, "%s on GPIO%d failed: %s", name, gpio, esp_err_to_name(err));
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    color_scale_init();

    add_strip(CONFIG_SCANNER_LED_GPIO, CONFIG_SCANNER_LED_COUNT, LED_ORDER, "Pixel output");
#if CONFIG_SCANNER_ONBOARD_LED
    if (CONFIG_SCANNER_ONBOARD_LED_GPIO == CONFIG_SCANNER_LED_GPIO) {
        ESP_LOGW(TAG, "Built-in LED GPIO is the same as the pixel GPIO; not mirroring");
    } else {
        add_strip(CONFIG_SCANNER_ONBOARD_LED_GPIO, 1,
                  IS_ONBOARD_RGB ? LED_ORDER_RGB : LED_ORDER_GRB, "Built-in LED");
    }
#endif
    if (s_num_strips > 0) {
        xTaskCreate(led_task, "led", 3072, NULL, 5, NULL);
    }

#if CONFIG_SCANNER_DISPLAY
    bool have_display = gfx_init() == ESP_OK;
    if (!have_display) {
        ESP_LOGE(TAG, "Display init failed; continuing without it");
    }
#endif
#if CONFIG_SCANNER_SD_LOG
    sdlog_init();
#endif
#if CONFIG_SCANNER_GPS
    gps_init();
#endif

    wifi_init();
#if CONFIG_SCANNER_DISPLAY
    wifi_loc_init();        /* location / FTM correction elements in beacons */
#endif
#if CONFIG_SCANNER_FTM
    ftm_init();
#endif
#if CONFIG_SCANNER_PHONE_GPS
    ble_gps_init();         /* phone GPS: advertise and accept a paired phone */
#endif
#if CONFIG_SCANNER_MODE_TARGET_SSID
    ESP_LOGI(TAG, "Tracking SSID \"%s\"", CONFIG_SCANNER_TARGET_SSID);
#else
    ESP_LOGI(TAG, "Tracking the strongest access point");
#endif
    s_lock = xSemaphoreCreateMutex();
    assert(s_lock);
    xTaskCreate(scan_task, "scan", 4096, NULL, 4, &s_scan_task);
#if CONFIG_SCANNER_DISPLAY
    if (have_display) {
        ui_init();
    }
#elif CONFIG_SCANNER_BUTTON
    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
#endif
}
