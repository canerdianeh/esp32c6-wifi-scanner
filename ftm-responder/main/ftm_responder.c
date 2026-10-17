/*
    Standalone Wi-Fi access point that answers FTM (802.11mc fine timing
    measurement) ranging requests, as a known-position beacon for the wearable
    Wi-Fi scanner.

    SSID: <prefix>-<last 4 hex digits of the AP MAC>, e.g. OnePingOnly-8641.
    The beacon advertises FTM responder, so the scanner ranges it in Wi-Fi
    details. No connection is needed for ranging.

    Status LED: heartbeat = running; lit (cyan on an RGB LED) = ranging requests
    are arriving, with a flash for each one. ESP-IDF has no responder-side FTM
    event, so requests are spotted by watching incoming management frames in
    promiscuous mode (802.11 public action frame, FTM Request).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "loc_ie.h"
#if CONFIG_RESPONDER_GPS
#include "gps.h"
#endif
#include "sdkconfig.h"
#if CONFIG_RESPONDER_LED_WS2812
#include "led_pixels.h"
#endif

static const char *TAG = "responder";

#define RANGING_ACTIVE_MS   3000    /* "ranging" while requests arrived this recently */

static int s_stations;
static uint8_t s_bssid[6];
static volatile uint32_t s_requests;        /* FTM requests received */
static volatile int64_t s_last_request_us;
static uint8_t s_last_initiator[6];

/* ANQP (802.11u): GAS Initial Requests are spotted by the sniffer and answered by anqp_task. */
#define ANQP_MAX_IDS 8
typedef struct {
    uint8_t sta[6];
    uint8_t token;
    uint8_t n;
    uint16_t ids[ANQP_MAX_IDS];
} anqp_req_t;

static QueueHandle_t s_anqp_q;
static volatile uint32_t s_anqp_count;
static portMUX_TYPE s_loc_mux = portMUX_INITIALIZER_UNLOCKED;
static loc_info_t s_cur_loc;        /* what the beacon advertises; also used for ANQP answers */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        s_stations++;
        ESP_LOGI(TAG, "Station joined: " MACSTR, MAC2STR(e->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        s_stations = s_stations > 0 ? s_stations - 1 : 0;
        ESP_LOGI(TAG, "Station left: " MACSTR, MAC2STR(e->mac));
    }
}

/* Runs in the Wi-Fi task: keep it short. */
static void on_promiscuous(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = buf;
    const uint8_t *f = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 26) {
        return;
    }
    int len = pkt->rx_ctrl.sig_len - 4;     /* without FCS */
    /* GAS Initial Request (public action 10) to us, with an ANQP query. */
    if ((f[0] & 0xFC) == 0xD0 && memcmp(f + 4, s_bssid, 6) == 0 && f[24] == 4 && f[25] == 10 && len >= 31 &&
        f[27] == 0x6C && s_anqp_q) {
        anqp_req_t r = { .token = f[26] };
        memcpy(r.sta, f + 10, 6);
        int i = 29 + f[28];                         /* after the Advertisement Protocol element */
        if (f[28] >= 2 && f[30] == 0 && i + 2 <= len) {     /* protocol ID 0 = ANQP */
            int end = i + 2 + (f[i] | (f[i + 1] << 8));
            end = end < len ? end : len;
            for (i += 2; i + 4 <= end;) {
                int id = f[i] | (f[i + 1] << 8), el = f[i + 2] | (f[i + 3] << 8);
                if (id == ANQP_QUERY_LIST) {
                    for (int k = 0; k + 1 < el && i + 4 + k + 1 < end && r.n < ANQP_MAX_IDS; k += 2) {
                        r.ids[r.n++] = f[i + 4 + k] | (f[i + 5 + k] << 8);
                    }
                }
                i += 4 + el;
            }
            xQueueSend(s_anqp_q, &r, 0);
        }
        return;
    }
    /* Action frame (type mgmt, subtype 13), addressed to us, category 4 (public), action 32 (FTM Request). */
    if ((f[0] & 0xFC) == 0xD0 && memcmp(f + 4, s_bssid, 6) == 0 && f[24] == 4 && f[25] == 32) {
        memcpy(s_last_initiator, f + 10, 6);
        s_last_request_us = esp_timer_get_time();
        s_requests++;
    }
}

/* Pick the antenna on boards with an RF switch (before Wi-Fi starts). */
static void select_antenna(void)
{
#if CONFIG_RESPONDER_ANTENNA_XIAO_INTERNAL || CONFIG_RESPONDER_ANTENNA_XIAO_EXTERNAL
    /* Seeed XIAO ESP32-C6: GPIO3 low powers the RF switch, GPIO14 picks the antenna (high = u.FL). */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << 3) | (1ULL << 14),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(3, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
#if CONFIG_RESPONDER_ANTENNA_XIAO_EXTERNAL
    gpio_set_level(14, 1);
    ESP_LOGI(TAG, "Antenna: external (u.FL)");
#else
    gpio_set_level(14, 0);
    ESP_LOGI(TAG, "Antenna: built-in");
#endif
#endif
}

/* Set one vendor element slot in beacons and probe responses (len 0 = remove). */
static void set_vendor_slot(wifi_vendor_ie_id_t slot, const uint8_t *ie, int len, bool was_set)
{
    if (was_set) {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, slot, NULL);
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, slot, NULL);
    }
    if (len) {
        esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, slot, ie);
        esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, slot, ie);
    }
}

/* Location and FTM correction in the beacon (see loc_ie.h): slot 0 = Aruba LCI
 * (RFC 3825 geo), slot 1 = civic address (RFC 4776) + FTM correction. Rewritten only when they change. */
static void update_beacon_info(void)
{
    static uint8_t s_last[2][256];
    static int s_last_len[2];

    loc_info_t loc = { 0 };
#if CONFIG_RESPONDER_GPS
    gps_info_t g;
    if (gps_get(&g) && g.valid) {
        loc.has_position = true;
        loc.lat = g.lat;
        loc.lon = g.lon;
        loc.has_alt = true;
        loc.alt_m = g.alt_m;
        loc.accuracy_m = gps_accuracy_m(&g);
        loc.has_source = true;
        loc.source = LOC_SOURCE_GPS;
    }
#endif
#if CONFIG_RESPONDER_LOC_FIXED
    if (!loc.has_position) {
        loc.has_position = true;
        loc.lat = atof(CONFIG_RESPONDER_LAT);
        loc.lon = atof(CONFIG_RESPONDER_LON);
        loc.has_alt = true;
        loc.alt_m = CONFIG_RESPONDER_ALT_M;
        loc.accuracy_m = CONFIG_RESPONDER_ACCURACY_M;
        loc.has_source = true;
        loc.source = LOC_SOURCE_FIXED;
    }
#endif
#if CONFIG_RESPONDER_CIVIC
    snprintf(loc.country, sizeof(loc.country), "%s", CONFIG_RESPONDER_CIVIC_COUNTRY);
    loc_civic_add(&loc, CA_A1, CONFIG_RESPONDER_CIVIC_A1);
    loc_civic_add(&loc, CA_A3, CONFIG_RESPONDER_CIVIC_A3);
    loc_civic_add(&loc, CA_RD, CONFIG_RESPONDER_CIVIC_RD);
    loc_civic_add(&loc, CA_HNO, CONFIG_RESPONDER_CIVIC_HNO);
    loc_civic_add(&loc, CA_PC, CONFIG_RESPONDER_CIVIC_PC);
    loc_civic_add(&loc, CA_BLD, CONFIG_RESPONDER_CIVIC_BLD);
    loc_civic_add(&loc, CA_FLR, CONFIG_RESPONDER_CIVIC_FLR);
    loc_civic_add(&loc, CA_ROOM, CONFIG_RESPONDER_CIVIC_ROOM);
    loc_civic_add(&loc, CA_NAM, CONFIG_RESPONDER_CIVIC_NAM);
    loc_civic_add(&loc, CA_LOC, CONFIG_RESPONDER_CIVIC_LOC);
#endif
    if (CONFIG_RESPONDER_FTM_OFFSET_CM != 0) {
        loc.has_ftm_cal = true;
        loc.ftm_cal_cm = CONFIG_RESPONDER_FTM_OFFSET_CM;
    }

    portENTER_CRITICAL(&s_loc_mux);
    s_cur_loc = loc;
    portEXIT_CRITICAL(&s_loc_mux);

    uint8_t ie[2][256];
    int len[2];
    len[0] = loc_build_aruba_lci(&loc, ie[0]);
    len[1] = loc_build_esp(&loc, ie[1], sizeof(ie[1]));
    bool changed = false;
    for (int s = 0; s < 2; s++) {
        if (len[s] != s_last_len[s] || memcmp(ie[s], s_last[s], len[s]) != 0) {
            set_vendor_slot(s == 0 ? WIFI_VND_IE_ID_0 : WIFI_VND_IE_ID_1, ie[s], len[s], s_last_len[s] > 0);
            memcpy(s_last[s], ie[s], len[s]);
            s_last_len[s] = len[s];
            changed = true;
        }
    }
    if (changed) {
        char civic[160];
        loc_civic_format(&loc, civic, sizeof(civic));
        if (loc.has_position) {
            ESP_LOGI(TAG, "Beacon: %s position %.6f, %.6f, %.0f m (Aruba LCI / RFC 3825)",
                     loc.source == LOC_SOURCE_GPS ? "GPS" : "fixed", loc.lat, loc.lon, loc.alt_m);
        }
        ESP_LOGI(TAG, "Beacon: civic \"%s\" (RFC 4776), FTM correction %d cm", civic, loc.ftm_cal_cm);
        for (int s = 0; s < 2; s++) {
            if (len[s]) {
                ESP_LOGI(TAG, "Element %d (%d bytes):", s, len[s]);
                ESP_LOG_BUFFER_HEX(TAG, ie[s], len[s]);
            }
        }
    }
}

static void anqp_put(uint8_t *out, int *n, uint16_t id, const uint8_t *payload, int len)
{
    out[(*n)++] = id & 0xFF;
    out[(*n)++] = id >> 8;
    out[(*n)++] = len & 0xFF;
    out[(*n)++] = len >> 8;
    memcpy(out + *n, payload, len);
    *n += len;
}

/* Answer ANQP queries for Capability List (257), AP Geospatial Location (265), AP Civic Location (266). */
static void anqp_task(void *arg)
{
    static uint8_t frame[600];
    uint8_t payload[260];
    anqp_req_t r;
    for (;;) {
        if (xQueueReceive(s_anqp_q, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        loc_info_t loc;
        portENTER_CRITICAL(&s_loc_mux);
        loc = s_cur_loc;
        portEXIT_CRITICAL(&s_loc_mux);

        int n = 0;
        frame[n++] = 0xD0;                  /* action frame */
        frame[n++] = 0;
        frame[n++] = 0;                     /* duration */
        frame[n++] = 0;
        memcpy(frame + n, r.sta, 6);        /* DA */
        n += 6;
        memcpy(frame + n, s_bssid, 6);      /* SA */
        n += 6;
        memcpy(frame + n, s_bssid, 6);      /* BSSID */
        n += 6;
        frame[n++] = 0;                     /* sequence (set by the driver) */
        frame[n++] = 0;
        frame[n++] = 4;                     /* public action */
        frame[n++] = 11;                    /* GAS Initial Response */
        frame[n++] = r.token;
        frame[n++] = 0;                     /* status: success */
        frame[n++] = 0;
        frame[n++] = 0;                     /* comeback delay */
        frame[n++] = 0;
        frame[n++] = 0x6C;                  /* Advertisement Protocol element: ANQP */
        frame[n++] = 2;
        frame[n++] = 0x7F;
        frame[n++] = 0;
        int qlen_at = n;
        n += 2;

        char asked[48] = "";
        int an = 0;
        for (int i = 0; i < r.n; i++) {
            an += snprintf(asked + an, sizeof(asked) - an, "%s%u", i ? "," : "", r.ids[i]);
            if (r.ids[i] == ANQP_CAPAB_LIST) {
                int pl = 0;
                uint16_t caps[3] = { ANQP_CAPAB_LIST, ANQP_AP_GEO_LOC, ANQP_AP_CIVIC_LOC };
                for (int c = 0; c < 3; c++) {
                    if ((caps[c] == ANQP_AP_GEO_LOC && !loc.has_position) || (caps[c] == ANQP_AP_CIVIC_LOC && !loc.country[0])) {
                        continue;
                    }
                    payload[pl++] = caps[c] & 0xFF;
                    payload[pl++] = caps[c] >> 8;
                }
                anqp_put(frame, &n, ANQP_CAPAB_LIST, payload, pl);
            } else if (r.ids[i] == ANQP_AP_GEO_LOC) {
                int pl = loc_anqp_geo_build(&loc, payload);
                if (pl) {
                    anqp_put(frame, &n, ANQP_AP_GEO_LOC, payload, pl);
                }
            } else if (r.ids[i] == ANQP_AP_CIVIC_LOC) {
                int pl = loc_anqp_civic_build(&loc, payload, sizeof(payload));
                if (pl) {
                    anqp_put(frame, &n, ANQP_AP_CIVIC_LOC, payload, pl);
                }
            }
        }
        int qlen = n - qlen_at - 2;
        frame[qlen_at] = qlen & 0xFF;
        frame[qlen_at + 1] = qlen >> 8;

        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, n, true);
        s_anqp_count++;
        ESP_LOGI(TAG, "ANQP query from " MACSTR " for %s: %s (%d bytes)", MAC2STR(r.sta), asked,
                 err == ESP_OK ? "answered" : esp_err_to_name(err), qlen);
        ESP_LOGI(TAG, "GAS Initial Response body (from the public action category):");
        ESP_LOG_BUFFER_HEX(TAG, frame + 24, n - 24);
    }
}

#if CONFIG_RESPONDER_LED
/* Status LED state, 20 updates per second:
 *   1 = flash (an FTM request just arrived), 2 = ranging, 3 = heartbeat blink, 0 = off. */
static int led_state(int tick, uint32_t *seen, int *flash)
{
    int64_t now = esp_timer_get_time();
    static uint32_t s_seen_anqp;
    uint32_t req = s_requests;
    if (req != *seen || s_anqp_count != s_seen_anqp) {
        *seen = req;
        s_seen_anqp = s_anqp_count;
        *flash = 3;                     /* 150 ms per FTM request or ANQP query */
    }
    if (*flash > 0) {
        (*flash)--;
        return 1;
    }
    if (s_last_request_us && now - s_last_request_us < RANGING_ACTIVE_MS * 1000LL) {
        return 2;
    }
    return tick % 40 < 3 ? 3 : 0;       /* short blink every 2 s */
}

static void led_task(void *arg)
{
#if CONFIG_RESPONDER_LED_WS2812
    led_pixels_handle_t led;
#if CONFIG_RESPONDER_LED_ORDER_RGB
    led_order_t order = LED_ORDER_RGB;
#else
    led_order_t order = LED_ORDER_GRB;
#endif
    if (led_pixels_new(CONFIG_RESPONDER_LED_GPIO, 1, order, &led) != ESP_OK) {
        ESP_LOGE(TAG, "Status LED on GPIO%d failed", CONFIG_RESPONDER_LED_GPIO);
        vTaskDelete(NULL);
    }
    const uint8_t b = CONFIG_RESPONDER_LED_BRIGHTNESS;
    const led_color_t colors[] = {
        { 0 },                                  /* off */
        { .g = b, .b = b },                     /* flash: bright cyan */
        { .g = b / 4, .b = b / 4 },             /* ranging: dim cyan */
        { .g = b / 2 },                         /* heartbeat: green */
    };
#else
    gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_RESPONDER_LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
#if CONFIG_RESPONDER_LED_ACTIVE_LOW
    const int on = 0;
#else
    const int on = 1;
#endif
#endif
    uint32_t seen = 0;
    int flash = 0;
    for (int tick = 0;; tick++) {
        int state = led_state(tick, &seen, &flash);
#if CONFIG_RESPONDER_LED_WS2812
        led_pixels_fill(led, colors[state]);
#else
        /* Single LED: on while ranging, briefly off for each request; blink when idle. */
        bool lit = state == 2 || state == 3 || (state == 1 && tick % 2);
        gpio_set_level(CONFIG_RESPONDER_LED_GPIO, lit ? on : !on);
#endif
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    select_antenna();
    s_anqp_q = xQueueCreate(4, sizeof(anqp_req_t));
    xTaskCreate(anqp_task, "anqp", 4096, NULL, 4, NULL);

#if CONFIG_RESPONDER_GPS
    gps_init();
#endif
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_read_mac(s_bssid, ESP_MAC_WIFI_SOFTAP));
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", CONFIG_RESPONDER_SSID_PREFIX, s_bssid[4], s_bssid[5]);

    wifi_config_t ap = {
        .ap = {
            .channel = CONFIG_RESPONDER_CHANNEL,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
            .ftm_responder = true,      /* advertise and answer FTM ranging requests */
        },
    };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap.ap.password, CONFIG_RESPONDER_PASSWORD, sizeof(ap.ap.password));
    if (strlen(CONFIG_RESPONDER_PASSWORD) < 8) {
        ESP_LOGW(TAG, "Password shorter than 8 characters; running as an open network");
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    /* 20 MHz only: 40 MHz channels on 2.4 GHz overlap most of the band. */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_RESPONDER_TX_POWER_DBM * 4));   /* units of 0.25 dBm */
    update_beacon_info();

    /* Watch management frames for FTM requests (for the LED and the log). */
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(on_promiscuous);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        ESP_LOGW(TAG, "Promiscuous mode unavailable; ranging won't show on the LED");
    }

#if CONFIG_RESPONDER_LED
    xTaskCreate(led_task, "led", 3072, NULL, 3, NULL);
#endif

    wifi_bandwidth_t bw = 0;
    esp_wifi_get_bandwidth(WIFI_IF_AP, &bw);
    ESP_LOGI(TAG, "FTM responder up: SSID \"%s\", BSSID " MACSTR ", channel %d, %s, correction %d cm",
             ssid, MAC2STR(s_bssid), CONFIG_RESPONDER_CHANNEL, bw == WIFI_BW20 ? "20 MHz" : "40 MHz",
             CONFIG_RESPONDER_FTM_OFFSET_CM);

    bool ranging = false;
    uint32_t last_count = 0;
    for (int sec = 1;; sec++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool active = s_last_request_us && esp_timer_get_time() - s_last_request_us < RANGING_ACTIVE_MS * 1000LL;
        if (active && !ranging) {
            ESP_LOGI(TAG, "Ranging started by " MACSTR, MAC2STR(s_last_initiator));
        } else if (!active && ranging) {
            ESP_LOGI(TAG, "Ranging stopped (%lu requests in total)", (unsigned long)s_requests);
        }
        ranging = active;
        if (sec % 5 == 0) {
            update_beacon_info();       /* follows the GPS position */
        }
        if (sec % 60 == 0) {
            ESP_LOGI(TAG, "Running: \"%s\", %d station(s), %lu FTM requests in the last minute", ssid, s_stations,
                     (unsigned long)(s_requests - last_count));
            last_count = s_requests;
        }
    }
}
