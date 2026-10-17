/*
    Location and FTM correction elements received in beacons; see wifi_loc.h.
    Reads the Aruba location element (LCI subtype 8 with an RFC 3825 geo LCI,
    or GPS ellipse subtype 9) and the Espressif-OUI civic / calibration
    element from the ftm-responder project.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_loc.h"

static const char *TAG = "wifi_loc";

#define MAX_ENTRIES 16
#define EXPIRE_MS   300000      /* same as the network list */

typedef struct {
    uint8_t bssid[6];
    loc_info_t loc;
    uint32_t seen_ms;
    uint32_t logged_sig;     /* what was last logged (presence flags, civic, correction) */
} entry_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static entry_t s_entries[MAX_ENTRIES];

/* ANQP queries (802.11u) for the BSSIDs on the open details page. */
#define QUERY_TARGETS       3
#define QUERY_AGAIN_MS      60000       /* re-ask an AP that answered */
#define QUERY_BACKOFF_MS    300000      /* skip an AP that didn't answer 3 times */
#define QUERY_WAIT_MS       400

typedef struct {
    uint8_t mac[6];
    uint8_t channel;
    uint32_t last_ok_ms;
    uint32_t skip_until_ms;
    uint8_t fails;
} query_t;

static query_t s_queries[QUERY_TARGETS];   /* guarded by s_mux */
static int s_num_queries;
static uint8_t s_sta_mac[6];
static SemaphoreHandle_t s_answered;
static volatile uint8_t s_pending_token;
static uint8_t s_pending_mac[6];
static uint8_t s_token;

static void on_gas_response(const uint8_t *sa, const uint8_t *b, int len);

/* Summary of the decoded content, ignoring coordinate jitter, to log only real changes. */
static uint32_t content_sig(const loc_info_t *l)
{
    uint32_t h = 2166136261u;
    uint8_t flags = l->has_position | (l->has_alt << 1) | (l->has_ftm_cal << 2) | (l->has_source << 3);
    const uint8_t parts[] = { flags, (uint8_t)l->ftm_cal_cm, (uint8_t)(l->ftm_cal_cm >> 8), (uint8_t)l->civic_n,
                              (uint8_t)l->country[0], (uint8_t)l->country[1], l->source };
    for (size_t i = 0; i < sizeof(parts); i++) {
        h = (h ^ parts[i]) * 16777619u;
    }
    for (int i = 0; i < l->civic_n; i++) {
        for (const char *c = l->civic[i].value; *c; c++) {
            h = (h ^ (uint8_t)*c) * 16777619u;
        }
    }
    return h | 1;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Called by the Wi-Fi driver for every vendor element in received beacons / probe responses. */
static void on_vendor_ie(void *ctx, wifi_vendor_ie_type_t type, const uint8_t sa[6], const vendor_ie_data_t *ie,
                         int rssi)
{
    const uint8_t *body = ie->vendor_oui;     /* OUI, OUI type, payload */
    bool aruba = body[0] == ARUBA_OUI_0 && body[1] == ARUBA_OUI_1 && body[2] == ARUBA_OUI_2;
    bool ours = body[0] == LOC_IE_OUI_0 && body[1] == LOC_IE_OUI_1 && body[2] == LOC_IE_OUI_2;
    if (!aruba && !ours) {
        return;
    }
    static loc_info_t s_tmp;    /* Wi-Fi task only */
    uint32_t now = now_ms();
    bool log_it = false;

    portENTER_CRITICAL(&s_mux);
    entry_t *e = NULL, *oldest = &s_entries[0];
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].seen_ms && memcmp(s_entries[i].bssid, sa, 6) == 0) {
            e = &s_entries[i];
            break;
        }
        if (s_entries[i].seen_ms < oldest->seen_ms) {
            oldest = &s_entries[i];
        }
    }
    s_tmp = e ? e->loc : (loc_info_t) { 0 };
    portEXIT_CRITICAL(&s_mux);

    if (!loc_parse_vendor_ie(body, ie->length, &s_tmp)) {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    if (!e) {
        e = oldest;
        memset(e, 0, sizeof(*e));
        memcpy(e->bssid, sa, 6);
    }
    e->loc = s_tmp;
    e->seen_ms = now;
    uint32_t sig = content_sig(&e->loc);
    if (sig != e->logged_sig && (loc_has_location(&e->loc) || e->loc.has_ftm_cal)) {
        e->logged_sig = sig;
        log_it = true;
    }
    portEXIT_CRITICAL(&s_mux);

    if (log_it) {
        char civic[120];
        loc_civic_format(&s_tmp, civic, sizeof(civic));
        ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x beacon: position %s %.6f %.6f, civic \"%s\", FTM correction %d cm",
                 sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], s_tmp.has_position ? "yes" : "no", s_tmp.lat, s_tmp.lon,
                 civic, s_tmp.ftm_cal_cm);
    }
}

/* Beacons and probe responses seen by the sniffer while scanning: walk their elements. */
static void on_promiscuous(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = buf;
    const uint8_t *f = pkt->payload;
    int len = pkt->rx_ctrl.sig_len - 4;     /* without FCS */
    uint8_t subtype = f[0] & 0xFC;
    if (subtype == 0xD0 && len >= 33 && f[24] == 4 && f[25] == 11 && s_pending_token &&
        memcmp(f + 4, s_sta_mac, 6) == 0) {
        on_gas_response(f + 10, f + 24, len - 24);      /* GAS Initial Response to us */
        return;
    }
    if ((subtype != 0x80 && subtype != 0x50) || len < 36) {     /* beacon, probe response */
        return;
    }
    const uint8_t *bssid = f + 16;
    for (int i = 36; i + 2 <= len;) {       /* 24-byte header + 12 fixed bytes */
        int id = f[i], el = f[i + 1];
        if (i + 2 + el > len) {
            break;
        }
        if (id == WIFI_VENDOR_IE_ELEMENT_ID && el >= 4) {
            on_vendor_ie(NULL, WIFI_VND_IE_TYPE_BEACON, bssid, (const vendor_ie_data_t *)(f + i), pkt->rx_ctrl.rssi);
        }
        i += 2 + el;
    }
}

/* Parse a GAS Initial Response carrying ANQP elements (body starts at the public action category). */
static void on_gas_response(const uint8_t *sa, const uint8_t *b, int len)
{
    /* category, action, token, status(2), comeback(2), Adv Protocol element, query length(2), ANQP elements */
    if (len < 9 || b[2] != s_pending_token || memcmp(sa, s_pending_mac, 6) != 0) {
        return;
    }
    int status = b[3] | (b[4] << 8);
    int i = 7;
    if (b[i] != 0x6C || i + 2 + b[i + 1] + 2 > len) {
        return;
    }
    i += 2 + b[i + 1];
    int end = i + 2 + (b[i] | (b[i + 1] << 8));
    end = end < len ? end : len;
    i += 2;

    static loc_info_t s_tmp;    /* Wi-Fi task only */
    bool got = false;
    portENTER_CRITICAL(&s_mux);
    entry_t *e = NULL, *oldest = &s_entries[0];
    for (int k = 0; k < MAX_ENTRIES; k++) {
        if (s_entries[k].seen_ms && memcmp(s_entries[k].bssid, sa, 6) == 0) {
            e = &s_entries[k];
            break;
        }
        if (s_entries[k].seen_ms < oldest->seen_ms) {
            oldest = &s_entries[k];
        }
    }
    s_tmp = e ? e->loc : (loc_info_t) { 0 };
    portEXIT_CRITICAL(&s_mux);

    while (status == 0 && i + 4 <= end) {
        int id = b[i] | (b[i + 1] << 8), el = b[i + 2] | (b[i + 3] << 8);
        if (i + 4 + el > end) {
            break;
        }
        if (id == ANQP_AP_GEO_LOC) {
            got |= loc_anqp_geo_parse(b + i + 4, el, &s_tmp);
        } else if (id == ANQP_AP_CIVIC_LOC) {
            got |= loc_anqp_civic_parse(b + i + 4, el, &s_tmp);
        }
        i += 4 + el;
    }
    if (got) {
        s_tmp.via_anqp = true;
        portENTER_CRITICAL(&s_mux);
        if (!e) {
            e = oldest;
            memset(e, 0, sizeof(*e));
            memcpy(e->bssid, sa, 6);
        }
        e->loc = s_tmp;
        e->seen_ms = now_ms();
        portEXIT_CRITICAL(&s_mux);
        char civic[120];
        loc_civic_format(&s_tmp, civic, sizeof(civic));
        ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x ANQP: position %s %.6f %.6f, civic \"%s\"", sa[0], sa[1],
                 sa[2], sa[3], sa[4], sa[5], s_tmp.has_position ? "yes" : "no", s_tmp.lat, s_tmp.lon, civic);
    }
    s_pending_token = 0;
    xSemaphoreGive(s_answered);     /* answered (even if it had no location) */
}

void wifi_loc_set_query_targets(const uint8_t (*macs)[6], const uint8_t *channels, int n)
{
    n = n > QUERY_TARGETS ? QUERY_TARGETS : n;
    portENTER_CRITICAL(&s_mux);
    query_t old[QUERY_TARGETS];
    int old_n = s_num_queries;
    memcpy(old, s_queries, sizeof(old));
    for (int i = 0; i < n; i++) {
        query_t q = { .channel = channels[i] };
        memcpy(q.mac, macs[i], 6);
        for (int k = 0; k < old_n; k++) {
            if (memcmp(old[k].mac, q.mac, 6) == 0) {
                q = old[k];     /* keep its query history */
                q.channel = channels[i];
            }
        }
        s_queries[i] = q;
    }
    s_num_queries = n;
    portEXIT_CRITICAL(&s_mux);
}

void wifi_loc_run_queries(void)
{
    for (int t = 0; t < QUERY_TARGETS; t++) {
        uint32_t now = now_ms();
        query_t q;
        portENTER_CRITICAL(&s_mux);
        bool due = t < s_num_queries && now >= s_queries[t].skip_until_ms &&
                   (!s_queries[t].last_ok_ms || now - s_queries[t].last_ok_ms >= QUERY_AGAIN_MS);
        if (due) {
            q = s_queries[t];
        }
        portEXIT_CRITICAL(&s_mux);
        if (!due) {
            continue;
        }

        /* GAS Initial Request: ANQP Query List for 257, 265, 266. */
        uint8_t f[48];
        int n = 0;
        f[n++] = 0xD0;
        f[n++] = 0;
        f[n++] = 0;
        f[n++] = 0;
        memcpy(f + n, q.mac, 6);
        n += 6;
        memcpy(f + n, s_sta_mac, 6);
        n += 6;
        memcpy(f + n, q.mac, 6);
        n += 6;
        f[n++] = 0;
        f[n++] = 0;
        s_token = s_token == 0xFF ? 1 : s_token + 1;
        const uint8_t body[] = { 4, 10, s_token, 0x6C, 2, 0x7F, 0, 10, 0,       /* query length 10 */
                                 0x00, 0x01, 6, 0,                           /* Query List, 6 octets */
                                 0x01, 0x01, 0x09, 0x01, 0x0A, 0x01 };       /* 257, 265, 266 */
        memcpy(f + n, body, sizeof(body));
        n += sizeof(body);

        memcpy(s_pending_mac, q.mac, 6);
        s_pending_token = s_token;
        xSemaphoreTake(s_answered, 0);
        esp_wifi_set_channel(q.channel, WIFI_SECOND_CHAN_NONE);
        bool ok = esp_wifi_80211_tx(WIFI_IF_STA, f, n, true) == ESP_OK &&
                  xSemaphoreTake(s_answered, pdMS_TO_TICKS(QUERY_WAIT_MS)) == pdTRUE;
        s_pending_token = 0;

        portENTER_CRITICAL(&s_mux);
        for (int k = 0; k < s_num_queries; k++) {
            if (memcmp(s_queries[k].mac, q.mac, 6) == 0) {
                if (ok) {
                    s_queries[k].last_ok_ms = now_ms();
                    s_queries[k].fails = 0;
                } else if (++s_queries[k].fails >= 3) {
                    s_queries[k].skip_until_ms = now_ms() + QUERY_BACKOFF_MS;
                    s_queries[k].fails = 0;
                }
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }
}

void wifi_loc_init(void)
{
    s_answered = xSemaphoreCreateBinary();
    esp_wifi_get_mac(WIFI_IF_STA, s_sta_mac);
    esp_wifi_set_vendor_ie_cb(on_vendor_ie, NULL);
    /* The vendor element callback doesn't report beacons seen while scanning, so also sniff them. */
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(on_promiscuous);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        ESP_LOGW(TAG, "Promiscuous mode unavailable; beacon locations won't be read");
    }
}

bool wifi_loc_get(const uint8_t bssid[6], loc_info_t *out)
{
    bool found = false;
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].seen_ms && memcmp(s_entries[i].bssid, bssid, 6) == 0 &&
            now - s_entries[i].seen_ms < EXPIRE_MS) {
            *out = s_entries[i].loc;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}
