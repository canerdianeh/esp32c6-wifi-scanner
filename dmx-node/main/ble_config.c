#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "node_config.h"
#include "net.h"
#include "ble_config.h"

static const char *TAG = "ble";

/* 6e0d0000-xxxx-4c1e-9d3a-a5c0ffee0d12, little endian for NimBLE. */
#define NODE_UUID(n) BLE_UUID128_INIT(0x12, 0x0d, 0xee, 0xff, 0xc0, 0xa5, 0x3a, 0x9d, \
                                      0x1e, 0x4c, (n) & 0xff, (n) >> 8, 0x00, 0x00, 0x0d, 0x6e)

static const ble_uuid128_t SVC_UUID = NODE_UUID(0x0001);
static const ble_uuid128_t CONFIG_UUID = NODE_UUID(0x0002);
static const ble_uuid128_t STATUS_UUID = NODE_UUID(0x0003);

static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_stop_pending;
static esp_timer_handle_t s_stop_timer;

static void advertise(void);

static int config_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* Long values are read in pieces; NimBLE applies the offset. */
        char *json = node_config_to_json(&g_config);
        if (!json) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        int rc = os_mbuf_append(ctxt->om, json, strlen(json));
        free(json);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* Writes longer than the MTU arrive whole, as a prepared ("long") write. */
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char *json = malloc(len);
        if (!json) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        ble_hs_mbuf_to_flat(ctxt->om, json, len, &len);
        esp_err_t err = node_config_apply_json(json, len);
        free(json);
        return err == ESP_OK ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    esp_netif_ip_info_t ip = net_ip_info();
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"ip\":\"" IPSTR "\",\"station\":%s}",
                     IP2STR(&ip.ip), net_is_station() ? "true" : "false");
    return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &CONFIG_UUID.u, .access_cb = config_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &STATUS_UUID.u, .access_cb = status_access, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

static void stop_ble(void *arg)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        s_stop_pending = true;  /* finish when the phone disconnects */
        return;
    }
    ESP_LOGI(TAG, "setup time over, Bluetooth off");
    nimble_port_stop();
    nimble_port_deinit();
    net_set_low_latency();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "disconnected");
        if (s_stop_pending) {
            /* Can't stop the host from its own task. */
            esp_timer_start_once(s_stop_timer, 1000);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields adv = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &SVC_UUID,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_hs_adv_fields rsp = {
        .name = (const uint8_t *)g_config.name,
        .name_len = strlen(g_config.name),
        .name_is_complete = 1,
    };
    ble_gap_adv_set_fields(&adv);
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "advertising: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising as \"%s\"", g_config.name);
}

static void host_task(void *arg)
{
    nimble_port_run();      /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_config_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_services);
    ble_gatts_add_svcs(s_services);
    ble_svc_gap_device_name_set(g_config.name);
    nimble_port_freertos_init(host_task);

    const esp_timer_create_args_t stop = { .callback = stop_ble, .name = "ble_stop" };
    esp_timer_create(&stop, &s_stop_timer);
    if (CONFIG_NODE_BLE_SETUP_MINUTES > 0 && g_config.wifi_ssid[0]) {
        esp_timer_start_once(s_stop_timer, CONFIG_NODE_BLE_SETUP_MINUTES * 60 * 1000000LL);
    }
    return ESP_OK;
}
