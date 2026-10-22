#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "node_config.h"
#include "net.h"

static const char *TAG = "net";

#define STA_RETRIES_BEFORE_AP 5
/* Each attempt to join scans all channels, which disturbs phones on the
 * setup access point, so once it's up retries are spaced out. */
#define STA_RETRY_WITH_AP_US (30 * 1000000LL)

#define EV_UP BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static volatile uint32_t s_generation;
static volatile bool s_sta_up;
static int s_retries;
static bool s_ap_on;
static esp_timer_handle_t s_retry_timer;

static void retry_connect(void *arg)
{
    esp_wifi_connect();
}

static void start_ap(void)
{
    if (s_ap_on) {
        return;
    }
    wifi_config_t ap = {
        .ap = {
            .channel = 6,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap.ap.ssid, g_config.name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, CONFIG_NODE_AP_PASSWORD, sizeof(ap.ap.password));

    /* Keep the station running alongside so it can still join later. */
    esp_wifi_set_mode(g_config.wifi_ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_on = true;
    s_generation++;
    xEventGroupSetBits(s_events, EV_UP);
    ESP_LOGI(TAG, "setup access point \"%s\", http://192.168.4.1/", g_config.name);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        if (++s_retries == STA_RETRIES_BEFORE_AP) {
            ESP_LOGW(TAG, "can't join \"%s\"", g_config.wifi_ssid);
            start_ap();
        }
        if (s_ap_on) {
            esp_timer_start_once(s_retry_timer, STA_RETRY_WITH_AP_US);
        } else {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "joined \"%s\" as " IPSTR, g_config.wifi_ssid, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        s_sta_up = true;
        s_generation++;
        xEventGroupSetBits(s_events, EV_UP);
    }
}

esp_err_t net_start(void)
{
    s_events = xEventGroupCreate();
    const esp_timer_create_args_t retry = { .callback = retry_connect, .name = "sta_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&retry, &s_retry_timer));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, g_config.name);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (g_config.wifi_ssid[0]) {
        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, g_config.wifi_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, g_config.wifi_pass, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_ERROR_CHECK(esp_wifi_start());
        start_ap();
    }
    return ESP_OK;
}

uint32_t net_wait_up(void)
{
    xEventGroupWaitBits(s_events, EV_UP, pdFALSE, pdTRUE, portMAX_DELAY);
    return s_generation;
}

uint32_t net_generation(void)
{
    return s_generation;
}

bool net_is_station(void)
{
    return s_sta_up;
}

esp_netif_ip_info_t net_ip_info(void)
{
    esp_netif_ip_info_t info = { 0 };
    esp_netif_get_ip_info(s_sta_up ? s_sta : s_ap, &info);
    return info;
}

void net_set_low_latency(void)
{
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi power saving off");
    } else {
        ESP_LOGW(TAG, "can't turn Wi-Fi power saving off: %s", esp_err_to_name(err));
    }
}
