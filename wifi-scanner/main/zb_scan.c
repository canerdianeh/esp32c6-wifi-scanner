/*
    IEEE 802.15.4 (Zigbee / Thread) scanner; see zb_scan.h.

    For each channel: an energy measurement, then a MAC beacon request, then
    ~150 ms listening (promiscuous) for beacons. Coordinators and routers that
    accept beacon requests answer with a beacon; Zigbee and Thread put their
    network details in the beacon payload.
*/
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "zb_scan.h"
#if CONFIG_SCANNER_DISPLAY
#include "ui.h"
#endif

static const char *TAG = "zigbee";

#define MAX_NETS        16
#define LISTEN_MS       150
#define ED_SYMBOLS      500         /* energy detect duration: 500 x 16 us = 8 ms */
#define ED_SYMBOLS_BAND 250         /* band mode: 4 ms per channel, ~12 sweeps/s */
#define NET_EXPIRE_MS   60000
#define MAX_RESPONDERS  8

typedef struct {
    uint8_t len;
    uint8_t data[127];
    int8_t rssi;
    uint8_t channel;
} rx_frame_t;

static QueueHandle_t s_rx_q;
static SemaphoreHandle_t s_ed_done, s_tx_done, s_stopped;
static volatile int8_t s_ed_power;
static volatile bool s_active;
static TaskHandle_t s_task;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int8_t s_energy[ZB_NUM_CH];          /* guarded by s_mux */
static uint16_t s_frames[ZB_NUM_CH];
static zb_net_t s_nets[MAX_NETS];
static uint16_t s_responders[MAX_NETS][MAX_RESPONDERS];  /* short/hashed source addresses */
static int s_num_nets;
static uint32_t s_sweeps;
static uint8_t s_seq;
static volatile bool s_beacons;             /* false = band mode: energy only, fast sweeps */
static int8_t s_avg[ZB_NUM_CH];             /* band mode: running average (dBm) */
static int8_t s_hist[ZB_HIST_ROWS][ZB_NUM_CH];  /* band mode: one row per sweep, ring buffer */
static int s_hist_head;
static int8_t s_sweep_raw[ZB_NUM_CH];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- 802.15.4 driver callbacks (ISR context) ---- */

IRAM_ATTR void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    if (s_rx_q) {
        rx_frame_t f;
        f.len = frame[0] > 127 ? 127 : frame[0];
        memcpy(f.data, frame + 1, f.len);
        f.rssi = info->rssi;
        f.channel = info->channel;
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_rx_q, &f, &woken);
        portYIELD_FROM_ISR(woken);
    }
    esp_ieee802154_receive_handle_done(frame);
}

IRAM_ATTR void esp_ieee802154_energy_detect_done(int8_t power)
{
    s_ed_power = power;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_ed_done, &woken);
    portYIELD_FROM_ISR(woken);
}

IRAM_ATTR void esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                            esp_ieee802154_frame_info_t *ack_frame_info)
{
    if (ack) {
        esp_ieee802154_receive_handle_done(ack);
    }
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

IRAM_ATTR void esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- beacon parsing ---- */

static void note_beacon(const rx_frame_t *f, uint16_t pan, uint16_t src, const uint8_t *pl, int pl_len,
                        uint16_t superframe)
{
    zb_proto_t proto = ZB_PROTO_OTHER;
    if (pl_len >= 15 && pl[0] == 0x00) {
        proto = ZB_PROTO_ZIGBEE;
    } else if (pl_len >= 2 && pl[0] == 0x03) {
        proto = ZB_PROTO_THREAD;
    }

    portENTER_CRITICAL(&s_mux);
    int idx = -1;
    for (int i = 0; i < s_num_nets; i++) {
        if (s_nets[i].pan_id == pan && s_nets[i].channel == f->channel) {
            idx = i;
            break;
        }
    }
    bool is_new = idx < 0;
    if (idx < 0) {
        if (s_num_nets < MAX_NETS) {
            idx = s_num_nets++;
        } else {
            idx = 0;
            for (int i = 1; i < s_num_nets; i++) {
                if (s_nets[i].last_seen_ms < s_nets[idx].last_seen_ms) {
                    idx = i;
                }
            }
        }
        memset(&s_nets[idx], 0, sizeof(s_nets[idx]));
        memset(s_responders[idx], 0, sizeof(s_responders[idx]));
        s_nets[idx].pan_id = pan;
        s_nets[idx].channel = f->channel;
        s_nets[idx].rssi = -127;
    }
    zb_net_t *n = &s_nets[idx];
    uint32_t now = now_ms();
    if (now - n->last_seen_ms > 5000) {
        n->rssi = -127;             /* new sweep: track the strongest answer afresh */
    }
    n->last_seen_ms = now;
    if (f->rssi > n->rssi) {
        n->rssi = f->rssi;
    }
    n->permit_join |= (superframe >> 15) & 1;
    if (proto != ZB_PROTO_OTHER) {
        n->proto = proto;
    }
    if (proto == ZB_PROTO_ZIGBEE) {
        n->stack_profile = pl[1] & 0x0F;
        n->router_capacity |= (pl[2] >> 2) & 1;
        memcpy(n->ext_pan, pl + 3, 8);
    } else if (proto == ZB_PROTO_THREAD && pl_len >= 18) {
        memcpy(n->name, pl + 2, 16);
        n->name[16] = 0;
    }
    for (int i = 0; i < MAX_RESPONDERS; i++) {
        if (s_responders[idx][i] == src) {
            break;
        }
        if (s_responders[idx][i] == 0) {
            s_responders[idx][i] = src;
            n->responders = i + 1;
            break;
        }
    }
    bool join = n->permit_join;
    portEXIT_CRITICAL(&s_mux);
    if (is_new) {
        ESP_LOGI(TAG, "Found %s PAN %04X on ch %d, %d dBm%s",
                 proto == ZB_PROTO_ZIGBEE ? "Zigbee" : proto == ZB_PROTO_THREAD ? "Thread" : "802.15.4",
                 pan, f->channel, f->rssi, join ? ", permit join open" : "");
    }
}

static void parse_frame(const rx_frame_t *f)
{
    int len = f->len - 2;           /* the last 2 bytes are RSSI/LQI, not FCS */
    const uint8_t *d = f->data;
    if (len < 3) {
        return;
    }
    int ch = f->channel - ZB_FIRST_CH;
    if (ch >= 0 && ch < ZB_NUM_CH) {
        portENTER_CRITICAL(&s_mux);
        s_frames[ch]++;
        portEXIT_CRITICAL(&s_mux);
    }

    uint16_t fcf = d[0] | (d[1] << 8);
    int type = fcf & 7, security = (fcf >> 3) & 1, pan_comp = (fcf >> 6) & 1;
    int dst_mode = (fcf >> 10) & 3, src_mode = (fcf >> 14) & 3;
    if (type != 0 || security || src_mode < 2) {
        return;                     /* only unsecured beacons */
    }
    int i = 3;
    if (dst_mode) {
        i += 2 + (dst_mode == 3 ? 8 : 2);
    }
    if (pan_comp && dst_mode) {
        return;                     /* beacons always carry their own PAN ID */
    }
    if (i + 2 > len) {
        return;
    }
    uint16_t pan = d[i] | (d[i + 1] << 8);
    i += 2;
    uint16_t src;
    if (src_mode == 2) {
        if (i + 2 > len) {
            return;
        }
        src = d[i] | (d[i + 1] << 8);
        i += 2;
    } else {
        if (i + 8 > len) {
            return;
        }
        src = 0;
        for (int k = 0; k < 8; k++) {
            src = (src * 31) ^ d[i + k];
        }
        i += 8;
    }
    if (src == 0) {
        src = 0xFFFE;
    }
    if (i + 4 > len) {
        return;
    }
    uint16_t superframe = d[i] | (d[i + 1] << 8);
    i += 2;
    int gts = d[i++] & 7;
    if (gts) {
        i += 1 + 3 * gts;
    }
    if (i >= len) {
        return;
    }
    uint8_t pend = d[i++];
    i += 2 * (pend & 7) + 8 * ((pend >> 4) & 7);
    if (i > len) {
        return;
    }
    note_beacon(f, pan, src, d + i, len - i, superframe);
}

/* ---- scanning task ---- */

static void send_beacon_request(void)
{
    /* FCF 0x0803: MAC command, dst short address, no src address. Leading byte = PHY length incl. FCS. */
    uint8_t frame[] = { 10, 0x03, 0x08, s_seq++, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x00 };
    xSemaphoreTake(s_tx_done, 0);
    if (esp_ieee802154_transmit(frame, false) == ESP_OK) {
        xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(50));
    }
}

static void scan_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* wait for start */
        esp_ieee802154_enable();
        esp_ieee802154_set_promiscuous(true);
        esp_ieee802154_set_rx_when_idle(true);
        esp_ieee802154_set_panid(0xFFFF);
        if (s_beacons) {
            ESP_LOGI(TAG, "802.15.4 scanning (beacons)");
        } else {
            ESP_LOGI(TAG, "802.15.4 scanning (band energy)");
        }

        while (s_active) {
            for (int c = 0; c < ZB_NUM_CH && s_active; c++) {
                esp_ieee802154_set_channel(ZB_FIRST_CH + c);

                xSemaphoreTake(s_ed_done, 0);
                if (esp_ieee802154_energy_detect(s_beacons ? ED_SYMBOLS : ED_SYMBOLS_BAND) == ESP_OK &&
                    xSemaphoreTake(s_ed_done, pdMS_TO_TICKS(50)) == pdTRUE) {
                    portENTER_CRITICAL(&s_mux);
                    /* Peak hold with slow decay, so short bursts stay visible. */
                    int8_t old = s_energy[c];
                    s_energy[c] = s_ed_power > old - (s_beacons ? 3 : 1) ? s_ed_power : old - (s_beacons ? 3 : 1);
                    s_avg[c] = (int8_t)((s_avg[c] * 7 + s_ed_power) / 8);
                    s_sweep_raw[c] = s_ed_power;
                    portEXIT_CRITICAL(&s_mux);
                }
                esp_ieee802154_receive();
                if (!s_beacons) {
                    continue;
                }

                send_beacon_request();
                esp_ieee802154_receive();

                int64_t until = esp_timer_get_time() + LISTEN_MS * 1000;
                rx_frame_t f;
                while (esp_timer_get_time() < until) {
                    if (xQueueReceive(s_rx_q, &f, pdMS_TO_TICKS(10)) == pdTRUE) {
                        parse_frame(&f);
                    }
                }
            }
            portENTER_CRITICAL(&s_mux);
            s_sweeps++;
            memcpy(s_hist[s_hist_head], s_sweep_raw, ZB_NUM_CH);
            s_hist_head = (s_hist_head + 1) % ZB_HIST_ROWS;
            int nets = s_num_nets, busiest = 0;
            for (int c = 1; c < ZB_NUM_CH; c++) {
                if (s_energy[c] > s_energy[busiest]) {
                    busiest = c;
                }
            }
            int8_t peak = s_energy[busiest];
            portEXIT_CRITICAL(&s_mux);
            if (s_beacons) {
                ESP_LOGI(TAG, "Sweep %lu: %d network(s), busiest ch %d at %d dBm", (unsigned long)s_sweeps, nets,
                         ZB_FIRST_CH + busiest, peak);
            } else if (s_sweeps % 50 == 0) {
                ESP_LOGI(TAG, "Band: busiest %d MHz at %d dBm (peak hold)", ZB_CH_MHZ(busiest), peak);
            }
#if CONFIG_SCANNER_DISPLAY
            if (s_beacons || s_sweeps % 2 == 0) {
                ui_request_redraw();
            }
#endif
        }

        /* Leave the radio idle before handing it back to Wi-Fi. */
        esp_ieee802154_set_rx_when_idle(false);
        esp_ieee802154_set_promiscuous(false);
        esp_ieee802154_sleep();
        esp_ieee802154_disable();
        xQueueReset(s_rx_q);
        xSemaphoreGive(s_stopped);
    }
}

void zb_scan_start(bool beacons)
{
    if (s_active && s_beacons != beacons) {
        zb_scan_stop();
    }
    s_beacons = beacons;
    if (!s_task) {
        s_rx_q = xQueueCreate(12, sizeof(rx_frame_t));
        s_ed_done = xSemaphoreCreateBinary();
        s_tx_done = xSemaphoreCreateBinary();
        s_stopped = xSemaphoreCreateBinary();
        xTaskCreate(scan_task, "zbscan", 4096, NULL, 4, &s_task);
    }
    if (!s_active) {
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < ZB_NUM_CH; i++) {
            s_energy[i] = s_avg[i] = s_sweep_raw[i] = -100;
        }
        memset(s_hist, -100, sizeof(s_hist));
        s_hist_head = 0;
        s_sweeps = 0;
        portEXIT_CRITICAL(&s_mux);
        s_active = true;
        xTaskNotifyGive(s_task);
    }
}

void zb_scan_stop(void)
{
    if (s_active) {
        s_active = false;
        xSemaphoreTake(s_stopped, pdMS_TO_TICKS(2000));
    }
}

void zb_scan_energy(int8_t energy[ZB_NUM_CH], uint16_t frames[ZB_NUM_CH])
{
    portENTER_CRITICAL(&s_mux);
    memcpy(energy, s_energy, sizeof(s_energy));
    memcpy(frames, s_frames, sizeof(s_frames));
    portEXIT_CRITICAL(&s_mux);
}

static int by_rssi(const void *a, const void *b)
{
    return ((const zb_net_t *)b)->rssi - ((const zb_net_t *)a)->rssi;
}

int zb_scan_networks(zb_net_t *out, int max)
{
    zb_net_t tmp[MAX_NETS];
    uint32_t now = now_ms();
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_num_nets; i++) {
        if (now - s_nets[i].last_seen_ms <= NET_EXPIRE_MS) {
            tmp[n++] = s_nets[i];
        }
    }
    portEXIT_CRITICAL(&s_mux);
    qsort(tmp, n, sizeof(tmp[0]), by_rssi);
    int count = n < max ? n : max;
    memcpy(out, tmp, count * sizeof(tmp[0]));
    return n;
}

uint32_t zb_scan_sweeps(void)
{
    return s_sweeps;
}

void zb_band_get(int8_t avg[ZB_NUM_CH], int8_t peak[ZB_NUM_CH], int8_t hist[][ZB_NUM_CH], int rows)
{
    portENTER_CRITICAL(&s_mux);
    memcpy(avg, s_avg, ZB_NUM_CH);
    memcpy(peak, s_energy, ZB_NUM_CH);
    for (int r = 0; r < rows && r < ZB_HIST_ROWS; r++) {
        int i = (s_hist_head - 1 - r + 2 * ZB_HIST_ROWS) % ZB_HIST_ROWS;     /* newest first */
        memcpy(hist[r], s_hist[i], ZB_NUM_CH);
    }
    portEXIT_CRITICAL(&s_mux);
}
