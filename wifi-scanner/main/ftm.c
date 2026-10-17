/*
    Wi-Fi FTM ranging; see ftm.h.

    Each session asks for 32 FTM frames. The driver reports an estimated
    distance plus per-frame round-trip times; the spread of those frames and
    of the last few sessions gives the uncertainty. APs that advertise FTM but
    refuse, or never answer, are skipped for a minute after repeated failures.
*/
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "ftm.h"
#include "wifi_loc.h"

static const char *TAG = "ftm";

#define MAX_PEERS       8
#define HISTORY         5
#define FRESH_MS        8000
#define SESSION_TIMEOUT_MS 3000
#define MAX_FAILS       3
#define BACKOFF_MS      60000
#define MAX_ENTRIES     32
#define MIN_UNCERTAINTY 1.0f        /* the wearable has a per-boot ranging offset of roughly +-0.9 m */
#define CM_PER_PS       0.015f      /* speed of light / 2 */

typedef struct {
    uint8_t mac[6];
    float dist_m[HISTORY];          /* ring of recent session distances */
    float frame_sd_m;               /* per-frame spread of the latest session */
    uint8_t count, head;
    uint8_t fails;
    uint32_t last_ok_ms;
    uint32_t skip_until_ms;
    uint32_t last_used_ms;
} peer_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ftm_target_t s_targets[FTM_MAX_TARGETS];     /* guarded by s_mux */
static int s_num_targets;
static peer_t s_peers[MAX_PEERS];                   /* guarded by s_mux */
static SemaphoreHandle_t s_done;
static wifi_event_ftm_report_t s_report;
static float s_report_frame_sd;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Find or create the peer for a MAC. Call with s_mux held. */
static peer_t *peer_for(const uint8_t *mac)
{
    peer_t *oldest = &s_peers[0];
    for (int i = 0; i < MAX_PEERS; i++) {
        if (memcmp(s_peers[i].mac, mac, 6) == 0) {
            return &s_peers[i];
        }
        if (s_peers[i].last_used_ms < oldest->last_used_ms) {
            oldest = &s_peers[i];
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    memcpy(oldest->mac, mac, 6);
    return oldest;
}

static void on_ftm_report(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_report = *(wifi_event_ftm_report_t *)data;
    s_report_frame_sd = -1;
    int n = s_report.ftm_report_num_entries;
    if (n > 0) {
        static wifi_ftm_report_entry_t entries[MAX_ENTRIES];
        if (n > MAX_ENTRIES) {
            n = MAX_ENTRIES;
        }
        if (esp_wifi_ftm_get_report(entries, n) == ESP_OK) {
            /* Standard deviation of the per-frame distances (ignoring invalid frames). */
            float sum = 0, sum2 = 0;
            int k = 0;
            for (int i = 0; i < n; i++) {
                if (entries[i].rtt == 0 || entries[i].rtt > 2000000) {     /* > 300 m: bogus */
                    continue;
                }
                float d = entries[i].rtt * CM_PER_PS / 100.0f;
                sum += d;
                sum2 += d * d;
                k++;
            }
            if (k >= 2) {
                float mean = sum / k;
                float var = sum2 / k - mean * mean;
                s_report_frame_sd = var > 0 ? sqrtf(var) : 0;
            }
        }
    }
    xSemaphoreGive(s_done);
}

void ftm_init(void)
{
    s_done = xSemaphoreCreateBinary();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, on_ftm_report, NULL);
}

void ftm_set_targets(const ftm_target_t *targets, int n)
{
    if (n > FTM_MAX_TARGETS) {
        n = FTM_MAX_TARGETS;
    }
    portENTER_CRITICAL(&s_mux);
    if (n > 0) {
        memcpy(s_targets, targets, n * sizeof(*targets));
    }
    s_num_targets = n < 0 ? 0 : n;
    portEXIT_CRITICAL(&s_mux);
}

static void run_session(const ftm_target_t *t)
{
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    peer_t *p = peer_for(t->mac);
    p->last_used_ms = now;
    bool skip = now < p->skip_until_ms;
    portEXIT_CRITICAL(&s_mux);
    if (skip) {
        return;
    }

    wifi_ftm_initiator_cfg_t cfg = {
        .channel = t->channel,
        .frm_count = 32,
        .burst_period = 2,      /* 200 ms */
    };
    memcpy(cfg.resp_mac, t->mac, 6);
    xSemaphoreTake(s_done, 0);
    esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    bool got = err == ESP_OK && xSemaphoreTake(s_done, pdMS_TO_TICKS(SESSION_TIMEOUT_MS)) == pdTRUE;
    if (err == ESP_OK && !got) {
        esp_wifi_ftm_end_session();
    }
    bool ok = got && s_report.status == FTM_STATUS_SUCCESS && memcmp(s_report.peer_mac, t->mac, 6) == 0;
    /* The responder may broadcast its own calibration in its beacon (loc_ie.h). */
    float raw_m = ok ? s_report.dist_est / 100.0f : 0;
    float cal_m = 0;
    loc_info_t loc;
    if (wifi_loc_get(t->mac, &loc) && loc.has_ftm_cal) {
        cal_m = loc.ftm_cal_cm / 100.0f;
    }
    float dist_m = raw_m - cal_m < 0 ? 0 : raw_m - cal_m;

    portENTER_CRITICAL(&s_mux);
    p = peer_for(t->mac);
    if (ok) {
        p->dist_m[p->head] = dist_m;
        p->head = (p->head + 1) % HISTORY;
        if (p->count < HISTORY) {
            p->count++;
        }
        p->frame_sd_m = s_report_frame_sd;
        p->last_ok_ms = now_ms();
        p->fails = 0;
    } else {
        /* Back off from APs that refuse, or never answer. A responder that has worked
         * before just gets retried: occasional empty sessions are normal. */
        bool refused = got && (s_report.status == FTM_STATUS_UNSUPPORTED || s_report.status == FTM_STATUS_CONF_REJECTED);
        if ((refused || p->last_ok_ms == 0) && ++p->fails >= MAX_FAILS) {
            p->skip_until_ms = now_ms() + BACKOFF_MS;
            p->fails = 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (ok) {
        ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x ch%d: %.2f m (raw %.2f m, correction %.2f m)", t->mac[0],
                 t->mac[1], t->mac[2], t->mac[3], t->mac[4], t->mac[5], t->channel, dist_m, raw_m, cal_m);
    } else {
        ESP_LOGW(TAG, "%02x:%02x:%02x:%02x:%02x:%02x: no measurement (%s, status %d)", t->mac[0], t->mac[1],
                 t->mac[2], t->mac[3], t->mac[4], t->mac[5], err != ESP_OK ? esp_err_to_name(err) : "report",
                 got ? (int)s_report.status : -1);
    }
}

void ftm_run_pending(void)
{
    ftm_target_t targets[FTM_MAX_TARGETS];
    portENTER_CRITICAL(&s_mux);
    int n = s_num_targets;
    memcpy(targets, s_targets, sizeof(targets));
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        run_session(&targets[i]);
    }
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

bool ftm_get(const uint8_t mac[6], ftm_result_t *out)
{
    float d[HISTORY];
    int n = 0;
    float frame_sd = -1;
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &s_peers[i];
        if (memcmp(p->mac, mac, 6) == 0 && p->count && now - p->last_ok_ms < FRESH_MS) {
            n = p->count;
            memcpy(d, p->dist_m, n * sizeof(float));
            frame_sd = p->frame_sd_m;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (n == 0) {
        return false;
    }

    qsort(d, n, sizeof(float), cmp_float);
    out->dist_m = n & 1 ? d[n / 2] : (d[n / 2 - 1] + d[n / 2]) / 2;
    /* Spread across sessions: standard deviation. */
    float mean = 0, var = 0;
    for (int i = 0; i < n; i++) {
        mean += d[i];
    }
    mean /= n;
    for (int i = 0; i < n; i++) {
        var += (d[i] - mean) * (d[i] - mean);
    }
    float session_sd = n >= 2 ? sqrtf(var / (n - 1)) : -1;
    /* Uncertainty: the larger of the two spreads; with one session and no frame data, assume moderate. */
    float u = session_sd > frame_sd ? session_sd : frame_sd;
    if (u < 0) {
        u = FTM_HIGH_ACCURACY_M;
    }
    out->uncertainty_m = u < MIN_UNCERTAINTY ? MIN_UNCERTAINTY : u;
    out->sessions = n;
    return true;
}
