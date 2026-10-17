/*
    Phone GPS over Bluetooth LE; see ble_gps.h.
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "sdkconfig.h"
#include "ble_host.h"
#include "ble_gps.h"
#include "gps.h"
#include "ui.h"

static const char *TAG = "ble_gps";

/* Nordic UART Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E; RX ...0002 (phone writes), TX ...0003 (notify). */
static const ble_uuid128_t s_nus_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static char s_name[20] = "WiFiScan";
static volatile ble_gps_state_t s_state = BLE_GPS_OFF;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint32_t s_bytes;
static volatile bool s_enabled;
static volatile bool s_subscribed;     /* phone has turned on TX notifications */
static uint32_t s_sent, s_dropped;

void ble_store_config_init(void);

static int gap_event(struct ble_gap_event *ev, void *arg);

/* GPS listener: send our position (module / USB / demo) to the phone as NMEA notifications on TX.
 * Never echo what the phone itself sent. Called from the GPS feeding tasks. */
static void send_nmea(const char *sentence, gps_source_t src)
{
    uint16_t conn = s_conn;
    if (src == GPS_SRC_PHONE || !s_subscribed || s_state != BLE_GPS_PAIRED || conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    char buf[112];
    int len = snprintf(buf, sizeof(buf), "%s\r\n", sentence);
    int chunk = ble_att_mtu(conn) - 3;
    if (chunk < 20) {
        chunk = 20;
    }
    for (int off = 0; off < len; off += chunk) {
        int n = len - off < chunk ? len - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf + off, n);
        if (!om || ble_gatts_notify_custom(conn, s_tx_handle, om) != 0) {
            s_dropped++;        /* out of buffers or link busy: drop the rest of this sentence */
            return;
        }
    }
    if (s_sent++ == 0) {
        ESP_LOGI(TAG, "Sending NMEA to the phone");
    }
}

static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[256];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (s_bytes == 0) {
            ESP_LOGI(TAG, "Receiving NMEA from the phone");
        }
        s_bytes += len;
        gps_feed(GPS_SRC_PHONE, buf, len);
        return 0;
    }
    return 0;   /* TX: notify only; reads return nothing */
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Encrypted + authenticated (MITM) writes only: unpaired phones get an
                 * "insufficient authentication" error, which makes them pair. */
                .uuid = &s_rx_uuid.u,
                .access_cb = nus_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = nus_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static void advertise(void)
{
    if (!s_enabled) {
        s_state = BLE_GPS_OFF;
        return;
    }
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = &s_nus_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    /* Slow advertising (~0.5 s) keeps airtime low next to Wi-Fi scanning. */
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(500),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(600),
    };
    if (rc == 0) {
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Advertising failed (%d)", rc);
        return;
    }
    s_state = BLE_GPS_ADVERTISING;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_state = BLE_GPS_CONNECTED;
            s_bytes = 0;
            s_subscribed = false;
            s_sent = s_dropped = 0;
            if (ble_gap_conn_find(s_conn, &desc) == 0) {
                const uint8_t *a = desc.peer_ota_addr.val;
                ESP_LOGI(TAG, "Connected: %02x:%02x:%02x:%02x:%02x:%02x%s", a[5], a[4], a[3], a[2], a[1], a[0],
                         desc.sec_state.bonded ? " (bonded)" : "");
            }
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason 0x%x; received %lu bytes, sent %lu sentences, %lu dropped)",
                 ev->disconnect.reason, (unsigned long)s_bytes, (unsigned long)s_sent, (unsigned long)s_dropped);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        ui_show_passkey(-1);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io pk = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000 };
            ESP_LOGI(TAG, "Pairing: enter passkey %06lu on the phone", (unsigned long)pk.passkey);
            ui_show_passkey((int32_t)pk.passkey);
            ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ui_show_passkey(-1);
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0) {
            s_state = BLE_GPS_PAIRED;
            ESP_LOGI(TAG, "Link encrypted (%s)", desc.sec_state.bonded ? "bonded" : "not bonded");
        } else {
            ESP_LOGW(TAG, "Pairing failed (%d)", ev->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = ev->subscribe.cur_notify;
            ESP_LOGI(TAG, "Phone %s NMEA notifications", s_subscribed ? "subscribed to" : "unsubscribed from");
            /* Our position goes only to a paired phone: ask an unpaired one to pair. */
            if (s_subscribed && s_state != BLE_GPS_PAIRED) {
                ble_gap_security_initiate(ev->subscribe.conn_handle);
            }
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The phone forgot us but we still have its old bond: drop it and pair again. */
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    uint8_t addr[6] = { 0 };
    if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) != 0) {
        ble_hs_id_copy_addr(BLE_ADDR_RANDOM, addr, NULL);
    }
    snprintf(s_name, sizeof(s_name), "WiFiScan-%02X%02X", addr[1], addr[0]);
    ble_svc_gap_device_name_set(s_name);
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && s_enabled) {
        advertise();
        ESP_LOGI(TAG, "Phone GPS: advertising as %s", s_name);
    }
}

void ble_gps_set_enabled(bool on)
{
    if (on == s_enabled) {
        return;
    }
    s_enabled = on;
    nvs_handle_t h;
    if (nvs_open("scanner", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "phone_gps", on);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Phone GPS %s", on ? "on" : "off");
    if (!ble_host_synced()) {
        return;
    }
    if (on) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
            ESP_LOGI(TAG, "Phone GPS: advertising as %s", s_name);
        }
    } else {
        ble_gap_adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        s_state = BLE_GPS_OFF;
    }
}

bool ble_gps_enabled(void)
{
    return s_enabled;
}

void ble_gps_init(void)
{
    nvs_handle_t h;
    uint8_t on = 0;
    if (nvs_open("scanner", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "phone_gps", &on);
        nvs_close(h);
    }
    s_enabled = on;
    if (!ble_host_init()) {
        return;
    }
    /* Passkey shown on our screen, typed on the phone (MITM-protected LE Secure Connections), bonded. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT service setup failed (%d)", rc);
        return;
    }
    ble_store_config_init();
    gps_set_listener(send_nmea);
    ble_host_on_sync(on_sync);
    ble_host_start();
}

ble_gps_state_t ble_gps_state(void)
{
    return s_enabled ? s_state : BLE_GPS_OFF;
}

const char *ble_gps_name(void)
{
    return s_name;
}
