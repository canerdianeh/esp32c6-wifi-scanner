/*
    Shared NimBLE host; see ble_host.h.
*/
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "ble_host.h"

static const char *TAG = "ble";

#define MAX_SYNC_CBS 4

static bool s_inited, s_started;
static volatile bool s_synced;
static void (*s_sync_cbs[MAX_SYNC_CBS])(void);
static int s_num_cbs;

static void on_sync(void)
{
    s_synced = true;
    for (int i = 0; i < s_num_cbs; i++) {
        s_sync_cbs[i]();
    }
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "BLE host reset (%d)", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_host_init(void)
{
    if (s_inited) {
        return true;
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    s_inited = true;
    return true;
}

void ble_host_start(void)
{
    if (s_inited && !s_started) {
        nimble_port_freertos_init(host_task);
        s_started = true;
    }
}

void ble_host_on_sync(void (*cb)(void))
{
    for (int i = 0; i < s_num_cbs; i++) {
        if (s_sync_cbs[i] == cb) {
            return;
        }
    }
    if (s_num_cbs < MAX_SYNC_CBS) {
        s_sync_cbs[s_num_cbs++] = cb;
    }
    if (s_synced) {
        cb();
    }
}

bool ble_host_synced(void)
{
    return s_synced;
}
